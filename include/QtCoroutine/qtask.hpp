#pragma once
#include <array>
#include <atomic>
#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <QFuture>
#include <QPromise>
#include "utils.hpp"

namespace QtCoroutine {

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
        auto final_suspend() noexcept {

            struct Awaiter {
                bool await_ready() noexcept { return false; }
                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> h) noexcept {
                    auto& p = h.promise();
                    return p.continuation ? p.continuation
                                          : std::noop_coroutine();
                }
                void await_resume() noexcept {}
            };

            return Awaiter{};
        }

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

        auto final_suspend() noexcept {

            struct Awaiter {
                bool await_ready() noexcept { return false; }
                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> h) noexcept {
                    auto& p = h.promise();
                    return p.continuation ? p.continuation
                                          : std::noop_coroutine();
                }
                void await_resume() noexcept {}
            };

            return Awaiter{};
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
// ------------------------------------------------------------------

template<typename... Ts>
    requires (sizeof...(Ts) > 0) && ((!std::is_void_v<Ts>) && ...)
QTask<std::tuple<Ts...>> whenAll(QTask<Ts> &... tasks) {
    co_return std::tuple{ (co_await tasks)... };
}

template<std::same_as<QTask<void>>... Tasks>
    requires (sizeof...(Tasks) > 0)
QTask<void> whenAll(Tasks &... tasks) {
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
                handle.resume();
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

}  // namespace QtCoroutine
