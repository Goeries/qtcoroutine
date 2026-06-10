#pragma once
#include <atomic>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <QMutex>
#include <QObject>
#include <QThread>
#include <QTimer>
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

// Thread semantics (mirrors QObject::connect's AutoConnection default —
// the suspended coroutine is the receiver, resuming it is the slot):
//  - By default the coroutine resumes on the thread it suspended on,
//    regardless of which thread the sender lives on or emits from. Code
//    after the co_await runs on the same thread as the code before it.
//    A same-thread emission resumes synchronously inside the emit, exactly
//    like a direct-connected slot.
//  - .resumeOn(ctx) explicitly migrates: delivery and everything after the
//    co_await run on ctx's thread.
template<typename Sender, typename Signal, typename ReadyCheck = std::nullptr_t>
    requires std::derived_from<Sender, QObject>
class QSignalAwaitable {
    using Args = utils::SignalArgs<Signal>;
    using Tuple = typename Args::tuple_type;
    using StopCallback = std::stop_callback<std::function<void()>>;

    // Everything the resume callbacks touch lives here, on the heap, shared
    // between the awaitable and the callbacks. Callbacks never touch the
    // awaitable: with resumeOn() to another thread, a signal can fire — and
    // run the coroutine to completion, destroying this awaitable — while
    // await_suspend is still executing on the awaiting thread.
    struct ControlBlock {
        // Exactly-one-resume gate; also set by ~QSignalAwaitable so stale
        // queued resumes are dropped after the frame is destroyed.
        std::atomic<bool> resumed{false};
        std::optional<Tuple> result;
        std::optional<utils::AwaitCancelled::Reason> cancelReason;
        // Default resume context, created on the awaiting thread. shared_ptr
        // so queued resumes keep it alive until delivered.
        std::shared_ptr<QObject> defaultCtx;

        // Teardown state. Mutex-guarded: await_suspend writes these on the
        // awaiting thread while (with resumeOn) the destructor may already
        // be disarming on the resume thread.
        QMutex armMutex;
        QMetaObject::Connection signalConnection;
        QMetaObject::Connection senderDestroyedConnection;
        QMetaObject::Connection resumeCtxDestroyedConnection;
        std::unique_ptr<StopCallback> stopCallback;

        // Thread-safe: disconnect(Connection) may be called from any thread,
        // and ~stop_callback waits out a concurrently-running callback.
        void disarm() {
            QMutexLocker lock(&armMutex);
            QObject::disconnect(signalConnection);
            QObject::disconnect(senderDestroyedConnection);
            QObject::disconnect(resumeCtxDestroyedConnection);
            stopCallback.reset();
        }
    };

public:
    QSignalAwaitable(Sender * sender, Signal signal)
        : m_sender(sender),
          m_signal(signal) {}

    // Marks any in-flight resume stale, then tears down connections and the
    // stop callback. Ensures that if the coroutine frame is destroyed while
    // suspended (e.g. task goes out of scope), no callback or queued resume
    // fires into dead memory. Callbacks themselves never disarm — only this
    // destructor does — so a resume racing await_suspend never touches
    // connection state concurrently.
    ~QSignalAwaitable() {
        if (m_cb) {
            m_cb->resumed.store(true, std::memory_order_release);
            m_cb->disarm();
        }
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
        return {m_sender, m_signal, m_resumeCtx, std::move(m_stop), m_timeout, std::forward<F>(check)};
    }

    // ---- Awaitable interface ----

    bool await_ready() {
        if (m_stop.stop_requested()) {
            // Mark cancelled here: await_suspend never runs on the ready
            // path, so the stop-callback that normally sets this is skipped.
            m_cb->cancelReason = utils::AwaitCancelled::Stopped;
            return true;
        }

        if constexpr (!std::is_same_v<ReadyCheck, std::nullptr_t>) {
            auto checkResult = std::invoke(m_ready, m_sender);
            if constexpr (Args::count == 0) {
                return static_cast<bool>(checkResult);
            } else {
                if (checkResult) {
                    m_cb->result.emplace(std::move(*checkResult));
                    return true;
                }
            }
        }

        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        Q_ASSERT_X(QThread::currentThread()->eventDispatcher(), "co_await QSignalAwaitable",
                   "co_await requires a running event loop on the co_await thread");

        // The connection context determines the resume thread. Default: a
        // fresh QObject on the awaiting thread, so cross-thread emissions
        // marshal here and the coroutine never silently migrates (the
        // coroutine is the receiver — same rule as QObject::connect).
        QObject * ctx;
        if (m_resumeCtx) {
            Q_ASSERT_X(m_resumeCtx->thread()->eventDispatcher(), "co_await QSignalAwaitable",
                       "co_await requires a running event loop on the resumeOn() thread");
            ctx = m_resumeCtx;
        } else {
            m_cb->defaultCtx = std::make_shared<QObject>();
            ctx = m_cb->defaultCtx.get();
        }

        // Callbacks capture the control block, never `this` (see ControlBlock
        // doc). They do not disarm; the awaitable's destructor does, right
        // after resume (end of the co_await full-expression) or when the
        // frame is destroyed while suspended. Stale fires in that window are
        // dropped by the `resumed` gate.
        auto cb = m_cb;

        QMutexLocker arming(&cb->armMutex);

        // If resumeContext goes out of scope first, cancel the await.
        // No context argument: runs directly on ctx's thread, where the
        // destroyed signal is emitted.
        if (m_resumeCtx)
            cb->resumeCtxDestroyedConnection =
                QObject::connect(m_resumeCtx, &QObject::destroyed, [cb, handle]() mutable {
                    if (cb->resumed.exchange(true, std::memory_order_acq_rel))
                        return;
                    cb->cancelReason = utils::AwaitCancelled::ResumeContextDestroyed;
                    handle.resume();
                });

        // If sender goes out of scope first, cancel the await (delivered on
        // ctx's thread).
        cb->senderDestroyedConnection = QObject::connect(m_sender, &QObject::destroyed, ctx, [cb, handle]() mutable {
            if (cb->resumed.exchange(true, std::memory_order_acq_rel))
                return;
            cb->cancelReason = utils::AwaitCancelled::SenderDestroyed;
            handle.resume();
        });

        // If the stop token triggers, cancel the await. request_stop() may
        // run this on any thread; the resume is marshalled to ctx's thread.
        // Capturing cb keeps defaultCtx alive until the queued call lands.
        if (m_stop.stop_possible()) {
            cb->stopCallback = std::make_unique<StopCallback>(m_stop, [cb, handle, ctx]() mutable {
                // Early check (the authoritative one runs at delivery): skip
                // posting once another path already resumed, avoiding a call
                // on a potentially-destroyed resumeOn() ctx.
                if (cb->resumed.load(std::memory_order_acquire))
                    return;
                QMetaObject::invokeMethod(
                    ctx,
                    [cb, handle]() mutable {
                        if (cb->resumed.exchange(true, std::memory_order_acq_rel))
                            return;
                        cb->cancelReason = utils::AwaitCancelled::Stopped;
                        handle.resume();
                    },
                    Qt::QueuedConnection);
            });
        }

        // If timeout specified, arm a single-shot fire on ctx's thread.
        // Nothing to stop or destroy: a stale fire is dropped by the gate
        // and merely keeps cb alive until expiry.
        if (m_timeout) {
            QTimer::singleShot(*m_timeout, ctx, [cb, handle]() mutable {
                if (cb->resumed.exchange(true, std::memory_order_acq_rel))
                    return;
                cb->cancelReason = utils::AwaitCancelled::Timeout;
                handle.resume();
            });
        }

        // Armed last: from this point the signal can fire (and, with
        // resumeOn, resume the coroutine on another thread) at any moment.
        cb->signalConnection = QObject::connect(m_sender, m_signal, ctx, [cb, handle](auto &&... args) mutable {
            if (cb->resumed.exchange(true, std::memory_order_acq_rel))
                return;
            cb->result.emplace(std::forward<decltype(args)>(args)...);
            handle.resume();
        });
    }

    void await_resume()
        requires(Args::count == 0)
    {
        if (m_cb->cancelReason)
            throw utils::AwaitCancelled{*m_cb->cancelReason};
    }

    [[nodiscard("co_await result contains signal args")]]
    utils::ConnectResultT<Signal> await_resume()
        requires(Args::count > 0)
    {
        if (m_cb->cancelReason)
            throw utils::AwaitCancelled{*m_cb->cancelReason};

        if constexpr (Args::count == 1)
            return std::get<0>(std::move(*m_cb->result));
        else
            return std::move(*m_cb->result);
    }

private:
    // readyIf() constructs a QSignalAwaitable with a different ReadyCheck type
    template<typename S, typename Sig, typename RC>
        requires std::derived_from<S, QObject>
    friend class QSignalAwaitable;

    // Forwards ALL builder state: readyIf() rebuilds the awaitable with a
    // different ReadyCheck type, so anything configured before it must be
    // carried over here or it is silently lost.
    QSignalAwaitable(Sender * sender, Signal signal, QObject * resumeCtx, std::stop_token stop,
                     std::optional<std::chrono::milliseconds> timeout, ReadyCheck ready)
        : m_sender(sender),
          m_signal(signal),
          m_resumeCtx(resumeCtx),
          m_stop(std::move(stop)),
          m_ready(std::move(ready)),
          m_timeout(timeout) {}

    Sender * m_sender;
    Signal m_signal;
    QObject * m_resumeCtx = nullptr;
    std::stop_token m_stop;
    [[no_unique_address]] ReadyCheck m_ready;
    std::optional<std::chrono::milliseconds> m_timeout;
    std::shared_ptr<ControlBlock> m_cb = std::make_shared<ControlBlock>();
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
