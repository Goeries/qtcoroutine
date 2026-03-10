#pragma once
#include <coroutine>
#include <QFuture>
#include <QFutureWatcher>

// These coroutine_traits specializations are allowed in `std` because coroutine_traits is meant to be extended
namespace std {
template<typename T, typename... Args>
struct coroutine_traits<QFuture<T>, Args...> {
    struct promise_type {
        QFuture<T> get_return_object() noexcept {
            return qpromise.future();
        }

        std::suspend_never initial_suspend() noexcept {
            qpromise.start();
            return {};
        }

        std::suspend_never final_suspend() noexcept {
            return {};
        }

        void return_value(const T & value) noexcept {
            qpromise.addResult(value);
            qpromise.finish();
        }

        void return_value(T && value) noexcept {
            qpromise.addResult(std::move(value));
            qpromise.finish();
        }

        void unhandled_exception() noexcept {
            qpromise.setException(std::current_exception());
            qpromise.finish();
        }

        QPromise<T> qpromise;
    };
};

// Void specialization
template<typename... Args>
struct coroutine_traits<QFuture<void>, Args...> {
    struct promise_type {
        QFuture<void> get_return_object() noexcept {
            return qpromise.future();
        }

        std::suspend_never initial_suspend() noexcept {
            qpromise.start();
            return {};
        }

        std::suspend_never final_suspend() noexcept {
            return {};
        }

        void return_void() noexcept {
            qpromise.finish();
        }

        void unhandled_exception() noexcept {
            qpromise.setException(std::current_exception());
            qpromise.finish();
        }

        QPromise<void> qpromise;
    };
};
}

// operator co_await for QFuture awaitable
template<typename T>
auto operator co_await(QFuture<T> future) {
    class Awaitable {
    public:
        Awaitable(QFuture<T> && future)
            : m_future(future)
        {}

        bool await_ready() const noexcept {
            return m_future.isFinished();
        }

        void await_suspend(std::coroutine_handle<> handle) {
            // m_handle = handle;  // Reserved for future use (e.g. cancellation support)

            // Lambda will only execute if watcher still exists (thus tied to lifetime of Awaitable instance)
            m_connection = QObject::connect(&m_watcher, &QFutureWatcher<T>::finished,
                                            &m_watcher, [handle]() {
                                                handle.resume();
                                            });

            m_watcher.setFuture(m_future);
        }

        // Qt wraps exceptions in QUnhandledException when propagating across threads via QFuture.
        // We unwrap and rethrow the original exception so callers get natural exception propagation.
        auto await_resume() {
            try {
                m_future.waitForFinished();
            } catch (QUnhandledException & e) {
                if (e.exception())
                    std::rethrow_exception(e.exception());
            }

            if constexpr (!std::is_void_v<T>) {
                if (m_future.isValid())
                    return m_future.takeResult();
                else
                    throw std::runtime_error("Awaitable cannot await_resume invalid future");
            }
        }

    private:
        // std::coroutine_handle<> m_handle;  // Reserved for future use
        QFuture<T> m_future;
        QFutureWatcher<T> m_watcher;
        QMetaObject::Connection m_connection;
    };

    return Awaitable {std::move(future)};
}
