#pragma once
#include <atomic>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <QException>
#include <stop_token>
#include "utils.hpp"

namespace QtCoroutine {

namespace detail {

template<typename F, typename Sender, typename Signal>
concept ValidReadyCheck = std::is_same_v<std::remove_cvref_t<F>, std::nullptr_t> ||
                          (std::invocable<F, Sender *> &&
                           std::convertible_to<std::invoke_result_t<F, Sender *>, utils::ReadyCheckResultT<Signal>>);

} // namespace detail

// ------------------------------------------------------------------
// ExpectedAwaitable – wraps any awaitable so await_resume returns
// std::expected<T, AwaitCancelled> instead of throwing.
// ------------------------------------------------------------------

template<typename Awaitable>
class ExpectedAwaitable {
    using ResumeT = decltype(std::declval<Awaitable &>().await_resume());

public:
    using ExpectedT = std::expected<ResumeT, utils::AwaitCancelled>;

    explicit ExpectedAwaitable(Awaitable && inner)
        : m_inner(std::move(inner)) {}

    bool await_ready() {
        return m_inner.await_ready();
    }

    auto await_suspend(std::coroutine_handle<> handle) {
        return m_inner.await_suspend(handle);
    }

    ExpectedT await_resume() {
        try {
            if constexpr (std::is_void_v<ResumeT>) {
                m_inner.await_resume();
                return {};
            } else {
                return m_inner.await_resume();
            }
        } catch (utils::AwaitCancelled & e) {
            return std::unexpected(e);
        }
    }

private:
    Awaitable m_inner;
};

// ------------------------------------------------------------------
// QSignalAwaitable – awaitable + builder for co_await-ing Qt signals
// ------------------------------------------------------------------

template<typename Sender, typename Signal, typename ReadyCheck = std::nullptr_t>
    requires std::derived_from<Sender, QObject>
class QSignalAwaitable {
    using Args = utils::SignalArgs<Signal>;
    using Tuple = typename Args::tuple_type;
    using StopCallback = std::stop_callback<std::function<void()>>;

public:
    QSignalAwaitable(Sender * sender, Signal signal)
        : m_sender(sender),
          m_signal(signal) {}

    // Destructor disconnects any active Qt connections. Ensures that if
    // the coroutine frame is destroyed while suspended (e.g. task goes out
    // of scope), signal callbacks don't fire into dead memory.
    ~QSignalAwaitable() {
        cleanup();
    }

    QSignalAwaitable(const QSignalAwaitable &) = delete;
    QSignalAwaitable & operator=(const QSignalAwaitable &) = delete;
    QSignalAwaitable(QSignalAwaitable &&) = default;
    QSignalAwaitable & operator=(QSignalAwaitable &&) = default;

    // ---- Builder methods (rvalue-qualified for safe chaining) ----

    QSignalAwaitable resumeOn(QObject * ctx) && {
        m_resumeCtx = ctx;
        return std::move(*this);
    }

    QSignalAwaitable cancelledBy(std::stop_token st) && {
        m_stop = std::move(st);
        return std::move(*this);
    }

    QSignalAwaitable withTimeout(std::chrono::milliseconds ms) && {
        m_timeout = ms;
        return std::move(*this);
    }

    ExpectedAwaitable<QSignalAwaitable> asExpected() && {
        return ExpectedAwaitable<QSignalAwaitable>(std::move(*this));
    }

    template<typename F>
        requires std::invocable<std::decay_t<F>, Sender *> &&
                 std::convertible_to<std::invoke_result_t<std::decay_t<F>, Sender *>, utils::ReadyCheckResultT<Signal>>
    QSignalAwaitable<Sender, Signal, std::decay_t<F>> readyIf(F && check) && {
        return {m_sender, m_signal, m_resumeCtx, std::move(m_stop), std::forward<F>(check)};
    }

    // ---- Awaitable interface ----

    bool await_ready() {
        if (m_stop.stop_requested()) {
            // Mark cancelled here: await_suspend never runs on the ready
            // path, so the stop-callback that normally sets this is skipped.
            m_cancelled = true;
            return true;
        }

        if constexpr (!std::is_same_v<ReadyCheck, std::nullptr_t>) {
            auto checkResult = std::invoke(m_ready, m_sender);
            if constexpr (Args::count == 0) {
                return static_cast<bool>(checkResult);
            } else {
                if (checkResult) {
                    m_result.emplace(std::move(*checkResult));
                    return true;
                }
            }
        }

        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        Q_ASSERT_X(QThread::currentThread()->eventDispatcher(), "co_await QSignalAwaitable",
                   "co_await requires a running event loop on the co_await thread");

        QObject * ctx = m_resumeCtx ? m_resumeCtx : m_sender;

        Q_ASSERT_X(ctx->thread()->eventDispatcher(), "co_await QSignalAwaitable",
                   "co_await requires a running event loop on the resuming thread");

        // Guard: multiple callbacks could fire near-simultaneously across
        // threads. Atomic exchange ensures exactly one calls handle.resume().
        // Stored as a member so cleanup()/the destructor can mark in-flight
        // resumes stale — see cleanup().
        m_guard = std::make_shared<std::atomic<bool>>(false);
        auto guard = m_guard;

        // If signal fires, resume coroutine
        m_signalConnection = QObject::connect(m_sender, m_signal, ctx, [this, handle, guard](auto &&... args) mutable {
            if (guard->exchange(true, std::memory_order_acq_rel))
                return;
            cleanup();
            m_result.emplace(std::forward<decltype(args)>(args)...);
            handle.resume();
        });

        // If sender goes out of scope first, clean up coroutine.
        // Using ctx ensures delivery on the correct thread when
        // resumeCtx lives on a different thread than sender.
        m_senderDestroyedConnection =
            QObject::connect(m_sender, &QObject::destroyed, ctx, [this, handle, guard]() mutable {
                if (guard->exchange(true, std::memory_order_acq_rel))
                    return;
                cleanup();
                m_senderDestroyed = true;
                handle.resume();
            });

        // If resumeContext goes out of scope first, clean up coroutine
        if (m_resumeCtx)
            m_resumeContextDestroyedConnection =
                QObject::connect(m_resumeCtx, &QObject::destroyed, [this, handle, guard]() mutable {
                    if (guard->exchange(true, std::memory_order_acq_rel))
                        return;
                    cleanup();
                    m_resumeContextDestroyed = true;
                    handle.resume();
                });

        // If coroutine cancelled first, clean up coroutine
        if (m_stop.stop_possible()) {
            m_stopCallback = std::make_unique<StopCallback>(m_stop, [this, handle, ctx, guard]() mutable {
                // Early check: skip invokeMethod if another callback already
                // won the race, avoiding a call on a potentially-destroyed ctx.
                if (guard->load(std::memory_order_acquire))
                    return;
                QMetaObject::invokeMethod(
                    ctx,
                    [this, handle, guard]() mutable {
                        if (guard->exchange(true, std::memory_order_acq_rel))
                            return;
                        cleanup();
                        m_cancelled = true;
                        handle.resume();
                    },
                    Qt::QueuedConnection);
            });
        }

        // If timeout specified, start timer
        if (m_timeout) {
            m_timeoutTimer = std::make_unique<QTimer>();
            m_timeoutTimer->setSingleShot(true);
            QObject::connect(m_timeoutTimer.get(), &QTimer::timeout, ctx, [this, handle, guard]() mutable {
                if (guard->exchange(true, std::memory_order_acq_rel))
                    return;
                cleanup();
                m_timedOut = true;
                handle.resume();
            });
            m_timeoutTimer->start(static_cast<int>(m_timeout->count()));
        }
    }

    void await_resume()
        requires(Args::count == 0)
    {
        if (m_cancelled)
            throw utils::AwaitCancelled{utils::AwaitCancelled::Stopped};
        else if (m_timedOut)
            throw utils::AwaitCancelled{utils::AwaitCancelled::Timeout};
        else if (m_senderDestroyed)
            throw utils::AwaitCancelled{utils::AwaitCancelled::SenderDestroyed};
        else if (m_resumeContextDestroyed)
            throw utils::AwaitCancelled{utils::AwaitCancelled::ResumeContextDestroyed};
    }

    [[nodiscard("co_await result contains signal args")]]
    utils::ConnectResultT<Signal> await_resume()
        requires(Args::count > 0)
    {
        if (m_cancelled)
            throw utils::AwaitCancelled{utils::AwaitCancelled::Stopped};
        else if (m_timedOut)
            throw utils::AwaitCancelled{utils::AwaitCancelled::Timeout};
        else if (m_senderDestroyed)
            throw utils::AwaitCancelled{utils::AwaitCancelled::SenderDestroyed};
        else if (m_resumeContextDestroyed)
            throw utils::AwaitCancelled{utils::AwaitCancelled::ResumeContextDestroyed};

        if constexpr (Args::count == 1)
            return std::get<0>(std::move(*m_result));
        else
            return std::move(*m_result);
    }

private:
    // readyIf() constructs a QSignalAwaitable with a different ReadyCheck type
    template<typename S, typename Sig, typename RC>
        requires std::derived_from<S, QObject>
    friend class QSignalAwaitable;

    QSignalAwaitable(Sender * sender, Signal signal, QObject * resumeCtx, std::stop_token stop, ReadyCheck ready)
        : m_sender(sender),
          m_signal(signal),
          m_resumeCtx(resumeCtx),
          m_stop(std::move(stop)),
          m_ready(std::move(ready)) {}

    void cleanup() {
        // Mark any in-flight resume as stale. The stop-token path posts its
        // resume via QMetaObject::invokeMethod (QueuedConnection); unlike the
        // connections below, a posted call cannot be disconnected and would
        // fire into freed memory if the coroutine frame is destroyed between
        // request_stop() and delivery.
        if (m_guard)
            m_guard->store(true, std::memory_order_release);
        QObject::disconnect(m_signalConnection);
        QObject::disconnect(m_senderDestroyedConnection);
        QObject::disconnect(m_resumeContextDestroyedConnection);
        m_stopCallback.reset();
        if (m_timeoutTimer)
            m_timeoutTimer->stop();
    }

    Sender * m_sender;
    Signal m_signal;
    QObject * m_resumeCtx = nullptr;
    std::stop_token m_stop;
    [[no_unique_address]] ReadyCheck m_ready;

    std::optional<Tuple> m_result;
    QMetaObject::Connection m_signalConnection;
    QMetaObject::Connection m_senderDestroyedConnection;
    QMetaObject::Connection m_resumeContextDestroyedConnection;
    std::unique_ptr<StopCallback> m_stopCallback;
    std::optional<std::chrono::milliseconds> m_timeout;
    std::unique_ptr<QTimer> m_timeoutTimer;
    std::shared_ptr<std::atomic<bool>> m_guard;
    bool m_cancelled = false;
    bool m_timedOut = false;
    bool m_senderDestroyed = false;
    bool m_resumeContextDestroyed = false;
};

// ------------------------------------------------------------------
// signal() – builder entry point (preferred API)
// ------------------------------------------------------------------

template<typename Sender, typename Signal>
    requires std::derived_from<Sender, QObject>
auto signal(Sender * sender, Signal sig) {
    return QSignalAwaitable<Sender, Signal>(sender, sig);
}

// ------------------------------------------------------------------
// connect() – alias for signal(), familiar to QObject/QtFuture users.
// Prefer signal() to avoid collisions with QtFuture::connect when
// both namespaces are imported via using-directives.
// ------------------------------------------------------------------

template<typename Sender, typename Signal>
    requires std::derived_from<Sender, QObject>
auto connect(Sender * sender, Signal sig) {
    return QSignalAwaitable<Sender, Signal>(sender, sig);
}

// ------------------------------------------------------------------
// sleep() – suspend for a duration (wraps QTimer::singleShot)
// ------------------------------------------------------------------

inline auto sleep(std::chrono::milliseconds ms) {
    struct SleepAwaitable {
        std::chrono::milliseconds duration;
        std::shared_ptr<std::atomic<bool>> guard;

        // If the coroutine frame is destroyed mid-sleep (task goes out of
        // scope), mark the pending timer callback stale so it doesn't
        // resume freed memory.
        ~SleepAwaitable() {
            if (guard)
                guard->store(true, std::memory_order_release);
        }

        bool await_ready() const noexcept {
            return duration <= std::chrono::milliseconds::zero();
        }

        void await_suspend(std::coroutine_handle<> handle) {
            Q_ASSERT_X(QThread::currentThread()->eventDispatcher(), "co_await sleep",
                       "co_await requires a running event loop on this thread");
            guard = std::make_shared<std::atomic<bool>>(false);
            QTimer::singleShot(duration, [handle, g = guard]() mutable {
                if (!g->exchange(true, std::memory_order_acq_rel))
                    handle.resume();
            });
        }

        void await_resume() const noexcept {}
    };

    return SleepAwaitable{ms};
}

} // namespace QtCoroutine
