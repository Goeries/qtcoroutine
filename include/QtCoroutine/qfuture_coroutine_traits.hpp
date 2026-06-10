#pragma once
#include <atomic>
#include <coroutine>
#include <memory>
#include <stdexcept>
#include <QFuture>
#include <QObject>
#include <QThread>
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

} // namespace std

// operator co_await for QFuture awaitable
template<typename T>
[[nodiscard]] auto operator co_await(QFuture<T> future) {
    class Awaitable {
    public:
        Awaitable(QFuture<T> && future)
            : m_future(std::move(future)) {}

        ~Awaitable() {
            if (m_guard)
                m_guard->store(true, std::memory_order_release);
        }

        bool await_ready() const noexcept {
            return m_future.isFinished();
        }

        // template<typename Promise>
        void await_suspend(std::coroutine_handle</*Promise*/> handle) {
            Q_ASSERT_X(QThread::currentThread()->eventDispatcher(), "co_await QFuture",
                       "co_await requires a running event loop on this thread");

            // Context is shared_ptr so the QFuture's continuation lambda keeps
            // the QObject alive until the callback fires — even if this Awaitable
            // (and its coroutine frame) is destroyed first (e.g. via timeout).
            // Guard prevents handle.resume() on a destroyed coroutine.
            auto context = std::make_shared<QObject>();
            m_guard = std::make_shared<std::atomic<bool>>(false);
            auto guard = m_guard;

            // Defer the resume to a fresh event-loop turn instead of
            // resuming synchronously from the continuation. A synchronous
            // resume drives the coroutine straight into its next co_await,
            // which re-enters QFutureInterfaceBase::setContinuation from
            // *inside* this continuation's delivery — the thread then blocks
            // on a lock it already holds further up its own stack
            // (self-deadlock). Posting the resume runs it after this delivery
            // has unwound.
            //
            // The guard check must run at resume time (inside the posted
            // lambda), not earlier: if the coroutine frame is destroyed during
            // the post->deliver gap (cancellation/timeout), the destructor
            // sets the guard and this gate drops the stale resume. Capturing
            // context in the posted lambda keeps the QObject alive until the
            // queued call is delivered (Qt drops queued calls whose context
            // has been destroyed).
            auto postResume = [handle, context, guard]() {
                // Early check (the authoritative one runs at delivery, below):
                // skip posting entirely once the frame is gone. This is not
                // just an optimization — when an abandoned await's marshalled
                // continuation event is destroyed during QCoreApplication
                // teardown, Qt runs the chained future's cancellation handler
                // synchronously while holding the post-event-list mutex;
                // calling postEvent from there self-deadlocks.
                if (guard->load(std::memory_order_acquire))
                    return;
                QMetaObject::invokeMethod(
                    context.get(),
                    [handle, context, guard]() mutable {
                        if (guard->exchange(true, std::memory_order_acq_rel))
                            return;
                        handle.resume();
                    },
                    Qt::QueuedConnection);
            };

            // The continuation must take QFuture<T>, not a value signature:
            // Qt never invokes value continuations when the parent future
            // holds an exception (the exception propagates to the chained
            // future instead), and then-continuations are skipped entirely on
            // plain cancellation — that's onCanceled's job. With a generic
            // callback here, a coroutine suspended on a future that is
            // canceled or throws would simply never resume.
            m_future.then(context.get(), [postResume](QFuture<T>) { postResume(); })
                .onCanceled(context.get(), [postResume] { postResume(); });

            // Note on QFuture cancellation:
            //
            // QFuture::cancel() is cooperative — it sets a flag but does not
            // preempt running work. This is a Qt limitation, not specific to
            // this library. Cancelling the awaited future resumes this
            // coroutine with AwaitCancelled{Stopped} via the onCanceled hook
            // above once the cancellation propagates through the continuation
            // chain (when the future settles: finish, QPromise destruction,
            // or the canceled worker completing). The underlying work keeps
            // running to completion either way. This matches native Qt
            // behavior (e.g. future.then() chains behave identically).
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
            // Rethrow stored exceptions before the isCanceled() check: a
            // future that failed with an exception also reports isCanceled(),
            // and checking cancellation first would misreport real errors as
            // AwaitCancelled. Only wait when already finished — a canceled
            // future may still have its (unstoppable) work running, and
            // waitForFinished() would block the resuming thread on it.
            if (m_future.isFinished()) {
                try {
                    m_future.waitForFinished();
                } catch (QUnhandledException & e) {
                    if (e.exception())
                        std::rethrow_exception(e.exception());
                    throw;
                }
            }

            if (m_future.isCanceled())
                throw QtCoroutine::utils::AwaitCancelled{QtCoroutine::utils::AwaitCancelled::Stopped};

            if constexpr (!std::is_void_v<T>) {
                if (m_future.isValid())
                    return m_future.takeResult();
                else
                    throw std::runtime_error("Awaitable cannot await_resume invalid future");
            } else
                return;
        }

    private:
        QFuture<T> m_future;
        std::shared_ptr<std::atomic<bool>> m_guard;
    };

    return Awaitable{std::move(future)};
}
