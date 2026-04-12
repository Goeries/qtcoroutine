#pragma once
#include <coroutine>
#include <QFuture>
#include <QFutureWatcher>
#include "utils.hpp"

// These coroutine_traits specializations are allowed in `std` because coroutine_traits is meant to be extended
namespace std {

template<typename T, typename... Args>
struct coroutine_traits<QFuture<T>, Args...> {
    struct promise_type {
        [[nodiscard]] QFuture<T> get_return_object() noexcept {
            return qpromise.future();
        }

        std::suspend_never initial_suspend() noexcept {
            qpromise.start();
            return {};
        }

        std::suspend_never final_suspend() noexcept {
            return {};
        }

        void return_value(const T & value) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            qpromise.addResult(value);
            qpromise.finish();
        }

        void return_value(T && value) noexcept(std::is_nothrow_move_constructible_v<T>) {
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
        [[nodiscard]] QFuture<void> get_return_object() noexcept {
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

}  // namespace std

// operator co_await for QFuture awaitable
template<typename T>
[[nodiscard]] auto operator co_await(QFuture<T> future) {
    class Awaitable {
    public:
        Awaitable(QFuture<T> && future)
            : m_future(std::move(future))
        {}

        ~Awaitable() {
            if (m_guard) m_guard->store(true, std::memory_order_release);
        }

        bool await_ready() const noexcept {
            return m_future.isFinished();
        }

        // template<typename Promise>
        void await_suspend(std::coroutine_handle</*Promise*/> handle) {
            Q_ASSERT_X(QThread::currentThread()->eventDispatcher(),
                       "co_await QFuture",
                       "co_await requires a running event loop on this thread");

            // Context is shared_ptr so the QFuture's continuation lambda keeps
            // the QObject alive until the callback fires — even if this Awaitable
            // (and its coroutine frame) is destroyed first (e.g. via timeout).
            // Guard prevents handle.resume() on a destroyed coroutine.
            auto context = std::make_shared<QObject>();
            m_guard = std::make_shared<std::atomic<bool>>(false);
            auto guard = m_guard;
            m_future.then(context.get(), [handle, context, guard](auto && ...) {
                if (guard->exchange(true, std::memory_order_acq_rel)) return;
                handle.resume();
            });

            // Note on QFuture cancellation:
            //
            // QFuture::cancel() is cooperative — it sets a flag but does not
            // preempt running work. This is a Qt limitation, not specific to
            // this library. If the outer QFuture is cancelled while the
            // coroutine is suspended on an inner QFuture, the inner work
            // runs to completion regardless. This matches native Qt behavior
            // (e.g. future.then() chains behave identically).
            //
            // For cancellable QFuture work, use the QPromise overload of
            // QtConcurrent::run and poll isCanceled():
            //
            //   QFuture<int> example() {
            //       auto f = QtConcurrent::run([](QPromise<int> & promise) {
            //           for (int i = 0; i < 1000 && !promise.isCanceled(); ++i)
            //               doChunk(i);
            //           if (!promise.isCanceled())
            //               promise.addResult(42);
            //       });
            //       co_return co_await f;
            //   }
            //
            // For signal-based workflows, prefer QtCoroutine::signal() with
            // .cancelledBy(stop_token) which provides true cancellation.
        }

        // Qt wraps exceptions in QUnhandledException when propagating across threads via QFuture.
        // We unwrap and rethrow the original exception so callers get natural exception propagation.
        auto await_resume() {
            if (m_future.isCanceled())
                throw QtCoroutine::utils::AwaitCancelled{
                    QtCoroutine::utils::AwaitCancelled::Stopped};

            try {
                m_future.waitForFinished();
            } catch (QUnhandledException & e) {
                if (e.exception())
                    std::rethrow_exception(e.exception());
                throw;
            }

            if constexpr (!std::is_void_v<T>) {
                if (m_future.isValid())
                    return m_future.takeResult();
                else
                    throw std::runtime_error("Awaitable cannot await_resume invalid future");
            }
            else
                return;
        }

    private:
        QFuture<T> m_future;
        std::shared_ptr<std::atomic<bool>> m_guard;
    };

    return Awaitable {std::move(future)};
}


