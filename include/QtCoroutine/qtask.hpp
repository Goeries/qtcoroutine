#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <tuple>
#include <utility>
#include <QFuture>
#include <QObject>
#include <QPromise>
#include <QThread>
#include <QTimer>
#include "utils.hpp"

namespace QtCoroutine {

// QTask<T> owns the coroutine frame — the destructor destroys it.
// Callbacks registered via .then()/.onCancelled()/.onError() fire from
// inside the coroutine (in return_value / unhandled_exception), so the
// frame must survive until completion. A common footgun:
//
//     someAsyncOp().then(cb);   // cb never fires — wrapper dies at ';'
//
// Two lifetime models avoid this:
//  1. co_await the task — the caller's coroutine frame keeps this one
//     alive across the suspension; no detach needed.
//  2. Fire-and-forget — register callbacks, then call .detach() to hand
//     ownership to the coroutine itself. The frame self-destructs after
//     final_suspend. Mirrors std::jthread::detach() semantics.
//
//         auto task = someAsyncOp();
//         task.then(cb);
//         task.detach();
//
// .detach() is not thread-safe (single-owner semantics).
template<typename T>
class QTask {

    enum class State {
        empty,
        value,
        cancelled,
        error
    };

public:
    struct promise_type {
        QTask get_return_object() {
            return QTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        // Eager start: coroutine runs to first suspension point immediately.
        // This ensures the coroutine frame is live before .then() is called.
        std::suspend_never initial_suspend() noexcept {
            return {};
        }

        // Symmetric transfer: resumes the parent coroutine (if any) when
        // this coroutine completes, avoiding stack overflow in deep chains.
        // If `detached` was set and nobody is awaiting us, await_ready
        // returns true so the coroutine proceeds past final_suspend and
        // the frame is destroyed by the coroutine ABI (self-destruct).
        auto final_suspend() noexcept {

            struct Awaiter {
                bool selfDestruct;
                bool await_ready() noexcept { return selfDestruct; }
                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> h) noexcept {
                    auto& p = h.promise();
                    return p.continuation ? p.continuation
                                          : std::noop_coroutine();
                }
                void await_resume() noexcept {}
            };

            return Awaiter{detached && !continuation};
        }

        // NOTE: QTask<T> only provides return_value(), not return_void().
        // The C++ standard forbids having both on the same promise type.
        // This means co_return; (no operand) won't compile for non-void T.
        // For types like std::expected<void, E>, use co_return {} instead
        // of co_return; to default-construct the success value.
        void return_value(const T & val) {
            result.emplace(val);
            state = State::value;
            if (thenCallback) {
                try { thenCallback(*result); } catch (...) {}
            }
        }

        void return_value(T && val) {
            result.emplace(std::move(val));
            state = State::value;
            if (thenCallback) {
                try { thenCallback(*result); } catch (...) {}
            }
        }

        void unhandled_exception() {
            try {
                throw;
            } catch (QtCoroutine::utils::AwaitCancelled & c) {
                // Cancellation is not an error, it's a state
                cancelReason = c.reason;
                state = State::cancelled;
                if (cancelledCallback) {
                    try { cancelledCallback(c); } catch (...) {}
                }
            } catch (...) {
                // Real errors propagate normally
                exception = std::current_exception();
                state = State::error;
                if (errorCallback) {
                    try { errorCallback(exception); } catch (...) {}
                }
            }
        }

        std::optional<T> result;
        std::exception_ptr exception;
        QtCoroutine::utils::AwaitCancelled::Reason cancelReason{};
        State state = State::empty;
        std::function<void(const T&)> thenCallback;
        std::function<void(const QtCoroutine::utils::AwaitCancelled &)> cancelledCallback;
        std::function<void(std::exception_ptr)> errorCallback;
        std::coroutine_handle<> continuation;
        bool detached = false;
    };

    explicit QTask(std::coroutine_handle<promise_type> handle)
        : m_handle(handle)
    {}

    ~QTask() {
        if (m_handle)
            m_handle.destroy();
    }

    QTask(const QTask & ) = delete;

    QTask(QTask && other) noexcept
        : m_handle(std::exchange(other.m_handle, nullptr))
    {}

    QTask & operator=(QTask && other) noexcept {
        if (this != &other) {
            if (m_handle)
                m_handle.destroy();
            m_handle = std::exchange(other.m_handle, nullptr);
        }
        return *this;
    }

    // See class doc for the fire-and-forget lifetime pattern.
    void detach() {
        if (!m_handle) return;
        if (m_handle.done()) {
            // Coroutine ran synchronously to completion and is now
            // suspended at final_suspend with selfDestruct=false (detached
            // was still false when the Awaiter was constructed). Setting
            // detached now is too late — destroy the frame manually.
            m_handle.destroy();
        } else {
            m_handle.promise().detached = true;
        }
        m_handle = {};
    }


    bool await_ready() const noexcept {
        return m_handle.done();
    }

    // Store continuation and suspend. The task's final_suspend will
    // resume us via symmetric transfer when the task completes.
    // We do NOT return m_handle (symmetric transfer to the task) because
    // with eager start, the task is already running and may be suspended
    // at an internal co_await — resuming it here would be UB.
    void await_suspend(std::coroutine_handle<> caller) {
        m_handle.promise().continuation = caller;
    }

    bool isCancelled() const {
        return m_handle.promise().state == State::cancelled;
    }

    QtCoroutine::utils::AwaitCancelled::Reason cancelReason() const {
        return m_handle.promise().cancelReason;
    }

    template<typename F>
    void then(F&& callback) {
        auto& p = m_handle.promise();
        if (m_handle.done()) {
            if (p.state == State::value)
                callback(*p.result);
        } else {
            p.thenCallback = std::forward<F>(callback);
        }
    }

    template<typename F>
    void onCancelled(F&& callback) {
        auto& p = m_handle.promise();
        if (m_handle.done()) {
            if (p.state == State::cancelled)
                callback(QtCoroutine::utils::AwaitCancelled{p.cancelReason});
        } else {
            p.cancelledCallback = std::forward<F>(callback);
        }
    }

    template<typename F>
    void onError(F&& callback) {
        auto& p = m_handle.promise();
        if (m_handle.done()) {
            if (p.state == State::error)
                callback(p.exception);
        } else {
            p.errorCallback = std::forward<F>(callback);
        }
    }

    // Bridge to QFuture — allows use with QFutureWatcher, QtFuture::whenAll, etc.
    // Consumes the .then/.onCancelled/.onError callbacks; use QFuture's
    // continuation API after calling this.
    QFuture<T> toFuture() {
        auto qpromise = std::make_shared<QPromise<T>>();
        qpromise->start();

        then([qpromise](const T & val) {
            qpromise->addResult(val);
            qpromise->finish();
        });

        onCancelled([qpromise](const QtCoroutine::utils::AwaitCancelled & c) {
            qpromise->setException(std::make_exception_ptr(c));
            qpromise->finish();
        });

        onError([qpromise](std::exception_ptr ep) {
            qpromise->setException(ep);
            qpromise->finish();
        });

        return qpromise->future();
    }

    T await_resume() {
        auto & p = m_handle.promise();
        switch (p.state) {
        case State::value:
            return std::move(*p.result);
        case State::cancelled:
            throw QtCoroutine::utils::AwaitCancelled{p.cancelReason};
        case State::error:
            std::rethrow_exception(p.exception);
        default:
            std::unreachable();
        }
    }

private:
    std::coroutine_handle<promise_type> m_handle;
};

// QTask<void> specialization
template <>
class QTask<void> {
    enum class State {
        empty,
        value,
        cancelled,
        error
    };

public:
    struct promise_type {
        QTask get_return_object() {
            return QTask { std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        std::suspend_never initial_suspend() noexcept {
            return {};
        }

        // See the QTask<T> specialization above for the semantics of
        // `selfDestruct` / `detached`.
        auto final_suspend() noexcept {

            struct Awaiter {
                bool selfDestruct;
                bool await_ready() noexcept { return selfDestruct; }
                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> h) noexcept {
                    auto& p = h.promise();
                    return p.continuation ? p.continuation
                                          : std::noop_coroutine();
                }
                void await_resume() noexcept {}
            };

            return Awaiter{detached && !continuation};
        }

        void return_void() noexcept {
            state = State::value;
            if (thenCallback) {
                try { thenCallback(); } catch (...) {}
            }
        }

        void unhandled_exception() {
            try {
                throw;
            } catch (QtCoroutine::utils::AwaitCancelled & c) {
                cancelReason = c.reason;
                state = State::cancelled;
                if (cancelledCallback) {
                    try { cancelledCallback(c); } catch (...) {}
                }
            } catch (...) {
                exception = std::current_exception();
                state = State::error;
                if (errorCallback) {
                    try { errorCallback(exception); } catch (...) {}
                }
            }
        }

        std::exception_ptr exception;
        QtCoroutine::utils::AwaitCancelled::Reason cancelReason{};
        State state = State::empty;
        std::function<void()> thenCallback;
        std::function<void(const QtCoroutine::utils::AwaitCancelled &)> cancelledCallback;
        std::function<void(std::exception_ptr)> errorCallback;
        std::coroutine_handle<> continuation;
        bool detached = false;
    };

    explicit QTask(std::coroutine_handle<promise_type> handle)
        : m_handle(handle)
    {}

    ~QTask() {
        if (m_handle)
            m_handle.destroy();
    }

    QTask(QTask && other) noexcept
        : m_handle(std::exchange(other.m_handle, nullptr))
    {}

    QTask & operator=(QTask && other) noexcept {
        if (this != &other) {
            if (m_handle)
                m_handle.destroy();
            m_handle = std::exchange(other.m_handle, nullptr);
        }
        return *this;
    }

    // See class doc (QTask<T> above) for the fire-and-forget pattern.
    void detach() {
        if (!m_handle) return;
        if (m_handle.done()) {
            m_handle.destroy();
        } else {
            m_handle.promise().detached = true;
        }
        m_handle = {};
    }

    bool await_ready() const noexcept {
        return m_handle.done();
    }

    void await_suspend(std::coroutine_handle<> caller) {
        m_handle.promise().continuation = caller;
    }

    void await_resume() {
        auto & p = m_handle.promise();
        switch (p.state) {
        case State::value:
            return;
        case State::cancelled:
            throw QtCoroutine::utils::AwaitCancelled{p.cancelReason};
        case State::error:
            std::rethrow_exception(p.exception);
        default:
            std::unreachable();
        }
    }

    bool isCancelled() const {
        return m_handle.promise().state == State::cancelled;
    }

    QtCoroutine::utils::AwaitCancelled::Reason cancelReason() const {
        return m_handle.promise().cancelReason;
    }

    template<typename F>
    void then(F&& callback) {
        auto& p = m_handle.promise();
        if (m_handle.done()) {
            if (p.state == State::value)
                callback();
        } else {
            p.thenCallback = std::forward<F>(callback);
        }
    }

    template<typename F>
    void onCancelled(F&& callback) {
        auto& p = m_handle.promise();
        if (m_handle.done()) {
            if (p.state == State::cancelled)
                callback(QtCoroutine::utils::AwaitCancelled{p.cancelReason});
        } else {
            p.cancelledCallback = std::forward<F>(callback);
        }
    }

    template<typename F>
    void onError(F&& callback) {
        auto& p = m_handle.promise();
        if (m_handle.done()) {
            if (p.state == State::error)
                callback(p.exception);
        } else {
            p.errorCallback = std::forward<F>(callback);
        }
    }

    QFuture<void> toFuture() {
        auto qpromise = std::make_shared<QPromise<void>>();
        qpromise->start();

        then([qpromise]() {
            qpromise->finish();
        });

        onCancelled([qpromise](const QtCoroutine::utils::AwaitCancelled & c) {
            qpromise->setException(std::make_exception_ptr(c));
            qpromise->finish();
        });

        onError([qpromise](std::exception_ptr ep) {
            qpromise->setException(ep);
            qpromise->finish();
        });

        return qpromise->future();
    }

    void start() {
        m_handle.resume();
    }

private:
    std::coroutine_handle<promise_type> m_handle;
};

// ------------------------------------------------------------------
// whenAll — co_await multiple QTasks, resume when all complete.
// Tasks are already running (eager start); this just collects results.
// Callback-based: uses .then/.onCancelled/.onError + atomic counter.
// Waits for ALL tasks before propagating any error or cancellation.
// ------------------------------------------------------------------

namespace detail {

template<typename... Ts>
struct WhenAllAwaitable {
    std::tuple<QTask<Ts>*...> tasks;

    bool await_ready() {
        return std::apply([](auto*... ptrs) {
            return (ptrs->await_ready() && ...);
        }, tasks);
    }

    void await_suspend(std::coroutine_handle<> handle) {
        constexpr auto N = sizeof...(Ts);
        auto remaining = std::make_shared<std::atomic<std::size_t>>(N);

        // The last task to complete resumes the outer coroutine.
        // If the task is already at final_suspend (done), resume directly.
        // Otherwise, set its continuation so final_suspend resumes us —
        // this avoids destroying a running coroutine (the callback fires
        // from return_value, before final_suspend).
        auto setupTask = [remaining, handle](auto* task) {
            auto onComplete = [remaining, handle, task]() {
                if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    if (task->await_ready())
                        handle.resume();
                    else
                        task->await_suspend(handle);
                }
            };
            task->then([onComplete](const auto&) { onComplete(); });
            task->onCancelled([onComplete](const auto&) { onComplete(); });
            task->onError([onComplete](auto) { onComplete(); });
        };

        std::apply([&setupTask](auto*... ptrs) {
            (setupTask(ptrs), ...);
        }, tasks);
    }

    std::tuple<Ts...> await_resume() {
        return std::apply([](auto*... ptrs) {
            return std::tuple<Ts...>{ ptrs->await_resume()... };
        }, tasks);
    }
};

template<std::size_t N>
struct WhenAllVoidAwaitable {
    std::array<QTask<void>*, N> tasks;

    bool await_ready() {
        for (auto* t : tasks)
            if (!t->await_ready()) return false;
        return true;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        auto remaining = std::make_shared<std::atomic<std::size_t>>(N);

        for (auto* task : tasks) {
            auto onComplete = [remaining, handle, task]() {
                if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    if (task->await_ready())
                        handle.resume();
                    else
                        task->await_suspend(handle);
                }
            };
            task->then([onComplete]() { onComplete(); });
            task->onCancelled([onComplete](const auto&) { onComplete(); });
            task->onError([onComplete](auto) { onComplete(); });
        }
    }

    void await_resume() {
        for (auto* t : tasks)
            t->await_resume();
    }
};

}  // namespace detail

template<typename... Ts>
    requires (sizeof...(Ts) > 0) && ((!std::is_void_v<Ts>) && ...)
auto whenAll(QTask<Ts> &... tasks) {
    return detail::WhenAllAwaitable<Ts...>{ std::tuple{&tasks...} };
}

template<std::same_as<QTask<void>>... Tasks>
    requires (sizeof...(Tasks) > 0)
auto whenAll(Tasks &... tasks) {
    return detail::WhenAllVoidAwaitable<sizeof...(Tasks)>{ {&tasks...} };
}

// ------------------------------------------------------------------
// tryAll — sequential co_await of multiple QTasks.
// Short-circuits on the first error or cancellation (fail-fast).
// Simpler than whenAll when you don't need all tasks to settle.
// ------------------------------------------------------------------

template<typename... Ts>
    requires (sizeof...(Ts) > 0) && ((!std::is_void_v<Ts>) && ...)
QTask<std::tuple<Ts...>> tryAll(QTask<Ts> &... tasks) {
    co_return std::tuple{ (co_await tasks)... };
}

template<std::same_as<QTask<void>>... Tasks>
    requires (sizeof...(Tasks) > 0)
QTask<void> tryAll(Tasks &... tasks) {
    ((co_await tasks), ...);
}

// ------------------------------------------------------------------
// whenAny — co_await multiple QTasks, resume when the first completes.
// Returns {index, result} of the winning task. Homogeneous types only.
// Consumes .then/.onCancelled/.onError callbacks on all tasks.
// ------------------------------------------------------------------

namespace detail {

template<typename T, std::size_t N>
struct WhenAnyAwaitable {
    std::array<QTask<T>*, N> tasks;
    std::size_t readyIndex = 0;

    bool await_ready() {
        for (std::size_t i = 0; i < N; ++i) {
            if (tasks[i]->await_ready()) {
                readyIndex = i;
                return true;
            }
        }
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        auto guard = std::make_shared<std::atomic<bool>>(false);
        for (std::size_t i = 0; i < N; ++i) {
            auto resume = [this, i, handle, guard]() {
                if (guard->exchange(true, std::memory_order_acq_rel)) return;
                readyIndex = i;
                if (tasks[i]->await_ready())
                    handle.resume();
                else
                    tasks[i]->await_suspend(handle);
            };
            tasks[i]->then([resume](const T &) { resume(); });
            tasks[i]->onCancelled([resume](const auto &) { resume(); });
            tasks[i]->onError([resume](auto) { resume(); });
        }
    }

    // Delegates to the winning task's await_resume — returns value,
    // or throws AwaitCancelled / rethrows on error.
    T await_resume() {
        return tasks[readyIndex]->await_resume();
    }
};

}  // namespace detail

template<typename T, typename... Rest>
    requires (std::same_as<QTask<T>, std::remove_cvref_t<Rest>> && ...)
auto whenAny(QTask<T> & first, Rest &... rest) {
    return detail::WhenAnyAwaitable<T, 1 + sizeof...(Rest)>{ {&first, &rest...} };
}

// ------------------------------------------------------------------
// cancelledBy — wrap a QTask with stop_token cancellation.
// Returns a new QTask that resolves with the original result or
// throws AwaitCancelled{Stopped} if the token is triggered first.
// The inner task continues running (fire-and-forget).
// ------------------------------------------------------------------

namespace detail {

template<typename T>
struct CancelledByAwaitable {
    using StopCallback = std::stop_callback<std::function<void()>>;

    QTask<T> & task;
    std::stop_token token;
    bool cancelled = false;
    std::shared_ptr<std::atomic<bool>> guard;
    std::unique_ptr<StopCallback> stopCb;
    std::unique_ptr<QObject> context;

    CancelledByAwaitable(QTask<T> & t, std::stop_token tok)
        : task(t), token(std::move(tok)) {}

    ~CancelledByAwaitable() {
        if (guard) guard->store(true, std::memory_order_release);
        stopCb.reset();
    }

    CancelledByAwaitable(CancelledByAwaitable &&) = default;
    CancelledByAwaitable & operator=(CancelledByAwaitable &&) = default;

    bool await_ready() {
        if (token.stop_requested()) {
            cancelled = true;
            return true;
        }
        return task.await_ready();
    }

    void await_suspend(std::coroutine_handle<> handle) {
        Q_ASSERT_X(QThread::currentThread()->eventDispatcher(),
                   "co_await cancelledBy",
                   "co_await requires a running event loop on this thread");

        guard = std::make_shared<std::atomic<bool>>(false);
        context = std::make_unique<QObject>();

        auto g = guard;
        auto ctx = context.get();

        auto resume = [this, handle, g, ctx](bool isCancelled) {
            if (g->load(std::memory_order_acquire)) return;
            QMetaObject::invokeMethod(ctx,
                [this, handle, g, isCancelled]() mutable {
                    if (g->exchange(true, std::memory_order_acq_rel)) return;
                    cancelled = isCancelled;
                    handle.resume();
                }, Qt::QueuedConnection);
        };

        if constexpr (std::is_void_v<T>) {
            task.then([resume]() { resume(false); });
        } else {
            task.then([resume](const T &) { resume(false); });
        }
        task.onCancelled([resume](const auto &) { resume(false); });
        task.onError([resume](auto) { resume(false); });

        if (token.stop_possible()) {
            stopCb = std::make_unique<StopCallback>(
                token, [resume]() { resume(true); });
        }
    }

    auto await_resume() -> decltype(task.await_resume()) {
        stopCb.reset();
        if (cancelled)
            throw utils::AwaitCancelled{utils::AwaitCancelled::Stopped};
        return task.await_resume();
    }
};

}  // namespace detail

template<typename T>
QTask<T> cancelledBy(QTask<T> & task, std::stop_token token) {
    if constexpr (std::is_void_v<T>) {
        co_await detail::CancelledByAwaitable<void>(task, std::move(token));
    } else {
        co_return co_await detail::CancelledByAwaitable<T>(task, std::move(token));
    }
}

// ------------------------------------------------------------------
// withTimeout — wrap a QTask with a timeout. Returns a new QTask
// that resolves with the original result OR throws AwaitCancelled{Timeout}.
// The inner task continues running after timeout (fire-and-forget).
// ------------------------------------------------------------------

namespace detail {

template<typename T>
struct TaskTimeoutAwaitable {
    QTask<T> & task;
    std::chrono::milliseconds ms;
    bool timedOut = false;
    std::unique_ptr<QTimer> timer;
    std::shared_ptr<std::atomic<bool>> guard;
    std::unique_ptr<QObject> context;

    TaskTimeoutAwaitable(QTask<T> & t, std::chrono::milliseconds m)
        : task(t), ms(m) {}

    ~TaskTimeoutAwaitable() {
        if (timer) timer->stop();
        if (guard) guard->store(true, std::memory_order_release);
    }

    TaskTimeoutAwaitable(TaskTimeoutAwaitable &&) = default;
    TaskTimeoutAwaitable & operator=(TaskTimeoutAwaitable &&) = default;

    bool await_ready() {
        return task.await_ready();
    }

    void await_suspend(std::coroutine_handle<> handle) {
        Q_ASSERT_X(QThread::currentThread()->eventDispatcher(),
                   "co_await withTimeout",
                   "co_await requires a running event loop on this thread");

        guard = std::make_shared<std::atomic<bool>>(false);
        context = std::make_unique<QObject>();

        auto g = guard;
        auto ctx = context.get();

        auto resume = [handle, g, ctx]() {
            if (g->load(std::memory_order_acquire)) return;
            QMetaObject::invokeMethod(ctx,
                [handle, g]() mutable {
                    if (g->exchange(true, std::memory_order_acq_rel)) return;
                    handle.resume();
                }, Qt::QueuedConnection);
        };

        if constexpr (std::is_void_v<T>) {
            task.then([resume]() { resume(); });
        } else {
            task.then([resume](const T &) { resume(); });
        }
        task.onCancelled([resume](const auto &) { resume(); });
        task.onError([resume](auto) { resume(); });

        timer = std::make_unique<QTimer>();
        timer->setSingleShot(true);
        QObject::connect(timer.get(), &QTimer::timeout, ctx,
                         [this, handle, g]() {
                             if (g->exchange(true, std::memory_order_acq_rel)) return;
                             timedOut = true;
                             handle.resume();
                         }, Qt::QueuedConnection);
        timer->start(static_cast<int>(ms.count()));
    }

    auto await_resume() -> decltype(task.await_resume()) {
        if (timedOut)
            throw utils::AwaitCancelled{utils::AwaitCancelled::Timeout};
        return task.await_resume();
    }
};

}  // namespace detail

template<typename T>
QTask<T> withTimeout(QTask<T> & task, std::chrono::milliseconds ms) {
    co_return co_await detail::TaskTimeoutAwaitable<T>(task, ms);
}

inline QTask<void> withTimeout(QTask<void> & task, std::chrono::milliseconds ms) {
    co_await detail::TaskTimeoutAwaitable<void>(task, ms);
}

}  // namespace QtCoroutine
