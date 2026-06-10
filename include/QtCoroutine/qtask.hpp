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
#include <QMutex>
#include <QObject>
#include <QPromise>
#include <QThread>
#include <QTimer>
#include "utils.hpp"

namespace QtCoroutine {

namespace detail {

// Completion dispatch, run from final_suspend's awaiter while the coroutine
// is suspended. Drains the registered callbacks in a loop — catching ones
// registered concurrently from another thread mid-dispatch — invokes them
// outside the lock, then, under the lock and atomically with publishing
// `settled`, decides the frame's destiny: self-destruct (detached), transfer
// to an awaiter, or stay parked for the owner.
//
// Because the coroutine is suspended here, a callback may destroy or
// reassign the owning QTask ("deleteLater from your own slot"): QTask's
// destructor detects the in-dispatch case and defers by setting `detached`,
// which this loop picks up before deciding the destiny.
template<typename Promise>
inline std::coroutine_handle<> dispatchCompletion(std::coroutine_handle<Promise> h) noexcept {
    auto & p = h.promise();
    while (true) {
        auto then = decltype(p.thenCallback){};
        auto cancelled = decltype(p.cancelledCallback){};
        auto error = decltype(p.errorCallback){};
        {
            QMutexLocker lock(&p.mutex);
            then = std::move(p.thenCallback);
            p.thenCallback = nullptr;
            cancelled = std::move(p.cancelledCallback);
            p.cancelledCallback = nullptr;
            error = std::move(p.errorCallback);
            p.errorCallback = nullptr;

            if (!then && !cancelled && !error) {
                p.settled->store(true, std::memory_order_release);
                if (p.detached && !p.continuation) {
                    lock.unlock();
                    h.destroy();
                    return std::noop_coroutine();
                }
                return p.continuation ? p.continuation : std::noop_coroutine();
            }
        }
        p.invokeCallbacks(then, cancelled, error);
    }
}

} // namespace detail

// QTask<T> owns the coroutine frame — the destructor destroys it.
// Callbacks registered via .then()/.onCancelled()/.onError() fire during
// completion dispatch at final_suspend, with the coroutine suspended, so a
// callback may destroy or reassign its own task (e.g. `m_task = next();`
// inside m_task's .then()) — frame destruction is deferred, like
// QObject::deleteLater() called from a slot. Note that after destroying the
// task inside a value callback, the callback's reference argument is no
// longer valid. The frame must survive until completion. A common footgun:
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
//
// co_await stores the awaiting coroutine's handle in this task's promise.
// At most one coroutine may await a given task (asserted in debug builds),
// and an awaited task must not outlive its awaiter: if the awaiting frame
// is destroyed while suspended, the task's completion would resume the
// destroyed frame. whenAll/whenAny guard against this internally; a plain
// co_await on a task you don't own cannot.
//
// Threading contract: a QTask belongs to the thread that created it. While
// the task is live, owner-side operations — co_await, then/onCancelled/
// onError, toFuture, detach, destruction — must happen on that thread
// (debug-asserted). Once settled, querying and destroying are safe from any
// thread that has synchronized with the completion. Inputs are always
// unrestricted: signals may be emitted, futures completed, and
// request_stop() called from any thread — the awaitables marshal the resume
// back to the right thread. After .resumeOn(ctx) migrates a coroutine to
// another thread, the original thread must treat the handle as foreign
// until settled (bridge results with toFuture()).
template<typename T>
class QTask {

    enum class State { empty, value, cancelled, error };

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

        // Completion dispatch (callbacks, settled flag, self-destruct or
        // symmetric transfer) happens in the awaiter, where the coroutine is
        // already suspended — see detail::dispatchCompletion.
        auto final_suspend() noexcept {

            struct Awaiter {
                bool await_ready() noexcept {
                    return false;
                }
                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                    return detail::dispatchCompletion(h);
                }
                void await_resume() noexcept {}
            };

            return Awaiter{};
        }

        // NOTE: QTask<T> only provides return_value(), not return_void().
        // The C++ standard forbids having both on the same promise type.
        // This means co_return; (no operand) won't compile for non-void T.
        // For types like std::expected<void, E>, use co_return {} instead
        // of co_return; to default-construct the success value.
        //
        // return_value/unhandled_exception only record the outcome; callback
        // dispatch happens at final_suspend.
        void return_value(const T & val) {
            QMutexLocker lock(&mutex);
            result.emplace(val);
            state = State::value;
        }

        void return_value(T && val) {
            QMutexLocker lock(&mutex);
            result.emplace(std::move(val));
            state = State::value;
        }

        void unhandled_exception() {
            QMutexLocker lock(&mutex);
            try {
                throw;
            } catch (QtCoroutine::utils::AwaitCancelled & c) {
                // Cancellation is not an error, it's a state
                cancelReason = c.reason;
                state = State::cancelled;
            } catch (...) {
                // Real errors propagate normally
                exception = std::current_exception();
                state = State::error;
            }
        }

        // Called by detail::dispatchCompletion with the coroutine suspended
        // and the mutex NOT held. state/result are stable — completion has
        // already been recorded. User-callback exceptions are swallowed
        // (final_suspend is noexcept).
        template<typename ThenFn, typename CancelFn, typename ErrorFn>
        void invokeCallbacks(ThenFn & then, CancelFn & cancelled, ErrorFn & error) noexcept {
            try {
                switch (state) {
                case State::value:
                    if (then)
                        then(*result);
                    break;
                case State::cancelled:
                    if (cancelled)
                        cancelled(QtCoroutine::utils::AwaitCancelled{cancelReason});
                    break;
                case State::error:
                    if (error)
                        error(exception);
                    break;
                default:
                    break;
                }
            } catch (...) {}
        }

        std::optional<T> result;
        std::exception_ptr exception;
        QtCoroutine::utils::AwaitCancelled::Reason cancelReason{};
        State state = State::empty;
        std::function<void(const T &)> thenCallback;
        std::function<void(const QtCoroutine::utils::AwaitCancelled &)> cancelledCallback;
        std::function<void(std::exception_ptr)> errorCallback;
        std::coroutine_handle<> continuation;
        bool detached = false;
        // Synchronizes state/result/callbacks/continuation against
        // cross-thread completion (e.g. after a resumeOn migration).
        mutable QMutex mutex;
        // Heap-shared so the QTask handle can still read it after a callback
        // destroyed the frame; true once completion dispatch has finished.
        std::shared_ptr<std::atomic<bool>> settled = std::make_shared<std::atomic<bool>>(false);
        // Creation thread; owner ops on a live task are asserted against it.
        QThread * ownerThread = QThread::currentThread();
    };

    explicit QTask(std::coroutine_handle<promise_type> handle)
        : m_handle(handle),
          m_settled(handle.promise().settled) {}

    ~QTask() {
        disposeHandle();
    }

    QTask(const QTask &) = delete;

    QTask(QTask && other) noexcept
        : m_handle(std::exchange(other.m_handle, nullptr)),
          m_settled(std::move(other.m_settled)) {}

    QTask & operator=(QTask && other) noexcept {
        if (this != &other) {
            disposeHandle();
            m_handle = std::exchange(other.m_handle, nullptr);
            m_settled = std::move(other.m_settled);
        }
        return *this;
    }

    // See class doc for the fire-and-forget lifetime pattern.
    void detach() {
        if (!m_handle)
            return;
        if (m_settled->load(std::memory_order_acquire)) {
            // Settled: dispatch already ran, the frame is parked at
            // final_suspend — destroy it now.
            { QMutexLocker barrier(&m_handle.promise().mutex); }
            m_handle.destroy();
        } else {
            // Pending — or mid-dispatch when called from this task's own
            // callback: either way the completion dispatch self-destructs
            // the frame.
            QMutexLocker lock(&m_handle.promise().mutex);
            m_handle.promise().detached = true;
        }
        m_handle = {};
    }

    bool await_ready() const noexcept {
        Q_ASSERT_X(m_handle, "QTask::await_ready", "no coroutine attached (moved-from or detached task)");
        // The settled flag (not m_handle.done()) so cross-thread observers
        // get an acquire-synchronized read that also covers the completion
        // dispatch — done() is true while callbacks are still running.
        return m_settled->load(std::memory_order_acquire);
    }

    // Store continuation and suspend. The task's final_suspend will
    // resume us via symmetric transfer when the task completes.
    // We do NOT return m_handle (symmetric transfer to the task) because
    // with eager start, the task is already running and may be suspended
    // at an internal co_await — resuming it here would be UB.
    bool await_suspend(std::coroutine_handle<> caller) {
        auto & p = m_handle.promise();
        QMutexLocker lock(&p.mutex);
        // Settled in the gap since await_ready (cross-thread completion):
        // don't park a continuation nobody will transfer to — resume now.
        if (m_settled->load(std::memory_order_acquire))
            return false;
        Q_ASSERT_X(!p.continuation, "QTask::co_await", "task is already awaited by another coroutine");
        p.continuation = caller;
        return true;
    }

    bool isCancelled() const {
        Q_ASSERT_X(m_handle, "QTask::isCancelled", "no coroutine attached (moved-from or detached task)");
        QMutexLocker lock(&m_handle.promise().mutex);
        return m_handle.promise().state == State::cancelled;
    }

    QtCoroutine::utils::AwaitCancelled::Reason cancelReason() const {
        Q_ASSERT_X(m_handle, "QTask::cancelReason", "no coroutine attached (moved-from or detached task)");
        QMutexLocker lock(&m_handle.promise().mutex);
        return m_handle.promise().cancelReason;
    }

    template<typename F>
    void then(F && callback) {
        Q_ASSERT_X(m_handle, "QTask::then", "no coroutine attached (moved-from or detached task)");
        auto & p = m_handle.promise();
        Q_ASSERT_X(m_settled->load(std::memory_order_acquire) || QThread::currentThread() == p.ownerThread,
                   "QTask::then", "register callbacks on a live task only from its owning thread");
        {
            QMutexLocker lock(&p.mutex);
            if (!m_settled->load(std::memory_order_acquire)) {
                p.thenCallback = std::forward<F>(callback);
                return; // pending — the completion dispatch will invoke it
            }
        }
        // Settled: dispatch has finished and state is immutable.
        if (p.state == State::value)
            callback(*p.result);
    }

    template<typename F>
    void onCancelled(F && callback) {
        Q_ASSERT_X(m_handle, "QTask::onCancelled", "no coroutine attached (moved-from or detached task)");
        auto & p = m_handle.promise();
        Q_ASSERT_X(m_settled->load(std::memory_order_acquire) || QThread::currentThread() == p.ownerThread,
                   "QTask::onCancelled", "register callbacks on a live task only from its owning thread");
        {
            QMutexLocker lock(&p.mutex);
            if (!m_settled->load(std::memory_order_acquire)) {
                p.cancelledCallback = std::forward<F>(callback);
                return;
            }
        }
        if (p.state == State::cancelled)
            callback(QtCoroutine::utils::AwaitCancelled{p.cancelReason});
    }

    template<typename F>
    void onError(F && callback) {
        Q_ASSERT_X(m_handle, "QTask::onError", "no coroutine attached (moved-from or detached task)");
        auto & p = m_handle.promise();
        Q_ASSERT_X(m_settled->load(std::memory_order_acquire) || QThread::currentThread() == p.ownerThread,
                   "QTask::onError", "register callbacks on a live task only from its owning thread");
        {
            QMutexLocker lock(&p.mutex);
            if (!m_settled->load(std::memory_order_acquire)) {
                p.errorCallback = std::forward<F>(callback);
                return;
            }
        }
        if (p.state == State::error)
            callback(p.exception);
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
        QMutexLocker lock(&p.mutex);
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
    // Single disposal path for the destructor and move-assignment.
    void disposeHandle() {
        if (!m_handle)
            return;
        if (m_settled->load(std::memory_order_acquire)) {
            // Settled: frame is parked at final_suspend. The empty locker
            // waits out the tail of a dispatch that published `settled` on
            // another thread moments ago.
            { QMutexLocker barrier(&m_handle.promise().mutex); }
            m_handle.destroy();
        } else if (m_handle.done()) {
            // Live but at final_suspend: we are inside this task's own
            // completion dispatch — a callback is destroying or reassigning
            // its task. Defer to the dispatcher (self-destruct), like
            // QObject::deleteLater() called from a slot.
            QMutexLocker lock(&m_handle.promise().mutex);
            m_handle.promise().detached = true;
        } else {
            Q_ASSERT_X(QThread::currentThread() == m_handle.promise().ownerThread, "QTask",
                       "a live QTask must be destroyed on its owning thread");
            m_handle.destroy();
        }
        m_handle = {};
    }

    std::coroutine_handle<promise_type> m_handle;
    std::shared_ptr<std::atomic<bool>> m_settled;
};

// QTask<void> specialization
template<>
class QTask<void> {
    enum class State { empty, value, cancelled, error };

public:
    struct promise_type {
        QTask get_return_object() {
            return QTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_never initial_suspend() noexcept {
            return {};
        }

        // See the QTask<T> specialization above: completion dispatch happens
        // in detail::dispatchCompletion, with the coroutine suspended.
        auto final_suspend() noexcept {

            struct Awaiter {
                bool await_ready() noexcept {
                    return false;
                }
                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                    return detail::dispatchCompletion(h);
                }
                void await_resume() noexcept {}
            };

            return Awaiter{};
        }

        void return_void() noexcept {
            QMutexLocker lock(&mutex);
            state = State::value;
        }

        void unhandled_exception() {
            QMutexLocker lock(&mutex);
            try {
                throw;
            } catch (QtCoroutine::utils::AwaitCancelled & c) {
                cancelReason = c.reason;
                state = State::cancelled;
            } catch (...) {
                exception = std::current_exception();
                state = State::error;
            }
        }

        // See QTask<T>::promise_type::invokeCallbacks.
        template<typename ThenFn, typename CancelFn, typename ErrorFn>
        void invokeCallbacks(ThenFn & then, CancelFn & cancelled, ErrorFn & error) noexcept {
            try {
                switch (state) {
                case State::value:
                    if (then)
                        then();
                    break;
                case State::cancelled:
                    if (cancelled)
                        cancelled(QtCoroutine::utils::AwaitCancelled{cancelReason});
                    break;
                case State::error:
                    if (error)
                        error(exception);
                    break;
                default:
                    break;
                }
            } catch (...) {}
        }

        std::exception_ptr exception;
        QtCoroutine::utils::AwaitCancelled::Reason cancelReason{};
        State state = State::empty;
        std::function<void()> thenCallback;
        std::function<void(const QtCoroutine::utils::AwaitCancelled &)> cancelledCallback;
        std::function<void(std::exception_ptr)> errorCallback;
        std::coroutine_handle<> continuation;
        bool detached = false;
        mutable QMutex mutex;
        std::shared_ptr<std::atomic<bool>> settled = std::make_shared<std::atomic<bool>>(false);
        QThread * ownerThread = QThread::currentThread();
    };

    explicit QTask(std::coroutine_handle<promise_type> handle)
        : m_handle(handle),
          m_settled(handle.promise().settled) {}

    ~QTask() {
        disposeHandle();
    }

    QTask(QTask && other) noexcept
        : m_handle(std::exchange(other.m_handle, nullptr)),
          m_settled(std::move(other.m_settled)) {}

    QTask & operator=(QTask && other) noexcept {
        if (this != &other) {
            disposeHandle();
            m_handle = std::exchange(other.m_handle, nullptr);
            m_settled = std::move(other.m_settled);
        }
        return *this;
    }

    // See class doc (QTask<T> above) for the fire-and-forget pattern.
    void detach() {
        if (!m_handle)
            return;
        if (m_settled->load(std::memory_order_acquire)) {
            {
                QMutexLocker barrier(&m_handle.promise().mutex);
            }
            m_handle.destroy();
        } else {
            QMutexLocker lock(&m_handle.promise().mutex);
            m_handle.promise().detached = true;
        }
        m_handle = {};
    }

    bool await_ready() const noexcept {
        Q_ASSERT_X(m_handle, "QTask::await_ready", "no coroutine attached (moved-from or detached task)");
        // The settled flag (not m_handle.done()) so cross-thread observers
        // get an acquire-synchronized read that also covers the completion
        // dispatch — done() is true while callbacks are still running.
        return m_settled->load(std::memory_order_acquire);
    }

    bool await_suspend(std::coroutine_handle<> caller) {
        auto & p = m_handle.promise();
        QMutexLocker lock(&p.mutex);
        // Settled in the gap since await_ready (cross-thread completion):
        // don't park a continuation nobody will transfer to — resume now.
        if (m_settled->load(std::memory_order_acquire))
            return false;
        Q_ASSERT_X(!p.continuation, "QTask::co_await", "task is already awaited by another coroutine");
        p.continuation = caller;
        return true;
    }

    void await_resume() {
        auto & p = m_handle.promise();
        QMutexLocker lock(&p.mutex);
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
        Q_ASSERT_X(m_handle, "QTask::isCancelled", "no coroutine attached (moved-from or detached task)");
        QMutexLocker lock(&m_handle.promise().mutex);
        return m_handle.promise().state == State::cancelled;
    }

    QtCoroutine::utils::AwaitCancelled::Reason cancelReason() const {
        Q_ASSERT_X(m_handle, "QTask::cancelReason", "no coroutine attached (moved-from or detached task)");
        QMutexLocker lock(&m_handle.promise().mutex);
        return m_handle.promise().cancelReason;
    }

    template<typename F>
    void then(F && callback) {
        Q_ASSERT_X(m_handle, "QTask::then", "no coroutine attached (moved-from or detached task)");
        auto & p = m_handle.promise();
        Q_ASSERT_X(m_settled->load(std::memory_order_acquire) || QThread::currentThread() == p.ownerThread,
                   "QTask::then", "register callbacks on a live task only from its owning thread");
        {
            QMutexLocker lock(&p.mutex);
            if (!m_settled->load(std::memory_order_acquire)) {
                p.thenCallback = std::forward<F>(callback);
                return; // pending — the completion dispatch will invoke it
            }
        }
        if (p.state == State::value)
            callback();
    }

    template<typename F>
    void onCancelled(F && callback) {
        Q_ASSERT_X(m_handle, "QTask::onCancelled", "no coroutine attached (moved-from or detached task)");
        auto & p = m_handle.promise();
        Q_ASSERT_X(m_settled->load(std::memory_order_acquire) || QThread::currentThread() == p.ownerThread,
                   "QTask::onCancelled", "register callbacks on a live task only from its owning thread");
        {
            QMutexLocker lock(&p.mutex);
            if (!m_settled->load(std::memory_order_acquire)) {
                p.cancelledCallback = std::forward<F>(callback);
                return;
            }
        }
        if (p.state == State::cancelled)
            callback(QtCoroutine::utils::AwaitCancelled{p.cancelReason});
    }

    template<typename F>
    void onError(F && callback) {
        Q_ASSERT_X(m_handle, "QTask::onError", "no coroutine attached (moved-from or detached task)");
        auto & p = m_handle.promise();
        Q_ASSERT_X(m_settled->load(std::memory_order_acquire) || QThread::currentThread() == p.ownerThread,
                   "QTask::onError", "register callbacks on a live task only from its owning thread");
        {
            QMutexLocker lock(&p.mutex);
            if (!m_settled->load(std::memory_order_acquire)) {
                p.errorCallback = std::forward<F>(callback);
                return;
            }
        }
        if (p.state == State::error)
            callback(p.exception);
    }

    QFuture<void> toFuture() {
        auto qpromise = std::make_shared<QPromise<void>>();
        qpromise->start();

        then([qpromise]() { qpromise->finish(); });

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

private:
    // Single disposal path for the destructor and move-assignment.
    void disposeHandle() {
        if (!m_handle)
            return;
        if (m_settled->load(std::memory_order_acquire)) {
            // Settled: frame is parked at final_suspend. The empty locker
            // waits out the tail of a dispatch that published `settled` on
            // another thread moments ago.
            { QMutexLocker barrier(&m_handle.promise().mutex); }
            m_handle.destroy();
        } else if (m_handle.done()) {
            // Live but at final_suspend: we are inside this task's own
            // completion dispatch — a callback is destroying or reassigning
            // its task. Defer to the dispatcher (self-destruct), like
            // QObject::deleteLater() called from a slot.
            QMutexLocker lock(&m_handle.promise().mutex);
            m_handle.promise().detached = true;
        } else {
            Q_ASSERT_X(QThread::currentThread() == m_handle.promise().ownerThread, "QTask",
                       "a live QTask must be destroyed on its owning thread");
            m_handle.destroy();
        }
        m_handle = {};
    }

    std::coroutine_handle<promise_type> m_handle;
    std::shared_ptr<std::atomic<bool>> m_settled;
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
    std::tuple<QTask<Ts> *...> tasks;
    std::shared_ptr<std::atomic<bool>> guard;

    // The callbacks registered on the (caller-owned) tasks outlive this
    // awaitable. If the awaiting coroutine frame is destroyed while
    // suspended, the destructor marks them stale so a later task
    // completion doesn't resume the destroyed frame.
    ~WhenAllAwaitable() {
        if (guard)
            guard->store(true, std::memory_order_release);
    }

    bool await_ready() {
        return std::apply([](auto *... ptrs) { return (ptrs->await_ready() && ...); }, tasks);
    }

    void await_suspend(std::coroutine_handle<> handle) {
        constexpr auto N = sizeof...(Ts);
        auto remaining = std::make_shared<std::atomic<std::size_t>>(N);
        guard = std::make_shared<std::atomic<bool>>(false);

        // The last task to complete resumes the outer coroutine.
        // If the task is already at final_suspend (done), resume directly.
        // Otherwise, set its continuation so final_suspend resumes us —
        // this avoids destroying a running coroutine (the callback fires
        // from return_value, before final_suspend).
        auto setupTask = [remaining, handle, g = guard](auto * task) {
            auto onComplete = [remaining, handle, g, task]() {
                if (g->load(std::memory_order_acquire))
                    return;
                if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    if (task->await_ready())
                        handle.resume();
                    else
                        task->await_suspend(handle);
                }
            };
            task->then([onComplete](const auto &) { onComplete(); });
            task->onCancelled([onComplete](const auto &) { onComplete(); });
            task->onError([onComplete](auto) { onComplete(); });
        };

        std::apply([&setupTask](auto *... ptrs) { (setupTask(ptrs), ...); }, tasks);
    }

    std::tuple<Ts...> await_resume() {
        return std::apply([](auto *... ptrs) { return std::tuple<Ts...>{ptrs->await_resume()...}; }, tasks);
    }
};

template<std::size_t N>
struct WhenAllVoidAwaitable {
    std::array<QTask<void> *, N> tasks;
    std::shared_ptr<std::atomic<bool>> guard;

    // See WhenAllAwaitable: drop stale completions after frame destruction.
    ~WhenAllVoidAwaitable() {
        if (guard)
            guard->store(true, std::memory_order_release);
    }

    bool await_ready() {
        for (auto * t : tasks)
            if (!t->await_ready())
                return false;
        return true;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        auto remaining = std::make_shared<std::atomic<std::size_t>>(N);
        guard = std::make_shared<std::atomic<bool>>(false);

        for (auto * task : tasks) {
            auto onComplete = [remaining, handle, g = guard, task]() {
                if (g->load(std::memory_order_acquire))
                    return;
                if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    if (task->await_ready())
                        handle.resume();
                    else
                        task->await_suspend(handle);
                }
            };
            task->then([onComplete]() { onComplete(); });
            task->onCancelled([onComplete](const auto &) { onComplete(); });
            task->onError([onComplete](auto) { onComplete(); });
        }
    }

    void await_resume() {
        for (auto * t : tasks)
            t->await_resume();
    }
};

} // namespace detail

template<typename... Ts>
    requires(sizeof...(Ts) > 0) && ((!std::is_void_v<Ts>) && ...)
auto whenAll(QTask<Ts> &... tasks) {
    return detail::WhenAllAwaitable<Ts...>{std::tuple{&tasks...}};
}

template<std::same_as<QTask<void>>... Tasks>
    requires(sizeof...(Tasks) > 0)
auto whenAll(Tasks &... tasks) {
    return detail::WhenAllVoidAwaitable<sizeof...(Tasks)>{{&tasks...}};
}

// ------------------------------------------------------------------
// tryAll — sequential co_await of multiple QTasks.
// Short-circuits on the first error or cancellation (fail-fast).
// Simpler than whenAll when you don't need all tasks to settle.
// ------------------------------------------------------------------

template<typename... Ts>
    requires(sizeof...(Ts) > 0) && ((!std::is_void_v<Ts>) && ...)
QTask<std::tuple<Ts...>> tryAll(QTask<Ts> &... tasks) {
    co_return std::tuple{(co_await tasks)...};
}

template<std::same_as<QTask<void>>... Tasks>
    requires(sizeof...(Tasks) > 0)
QTask<void> tryAll(Tasks &... tasks) {
    ((co_await tasks), ...);
}

// ------------------------------------------------------------------
// whenAny — co_await multiple QTasks, resume when the first completes.
// Returns the winning task's result (value, or rethrows its cancellation
// or error). Homogeneous types only.
// Consumes .then/.onCancelled/.onError callbacks on all tasks.
// ------------------------------------------------------------------

namespace detail {

template<typename T, std::size_t N>
struct WhenAnyAwaitable {
    std::array<QTask<T> *, N> tasks;
    std::size_t readyIndex = 0;
    std::shared_ptr<std::atomic<bool>> guard;

    // The guard doubles as the only-one-winner gate (exchange below) and
    // as the staleness flag: if the awaiting frame is destroyed while
    // suspended, the destructor sets it so the losing tasks' callbacks —
    // which capture `this` — never touch the destroyed awaitable/frame.
    ~WhenAnyAwaitable() {
        if (guard)
            guard->store(true, std::memory_order_release);
    }

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
        guard = std::make_shared<std::atomic<bool>>(false);
        for (std::size_t i = 0; i < N; ++i) {
            auto resume = [this, i, handle, g = guard]() {
                if (g->exchange(true, std::memory_order_acq_rel))
                    return;
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

} // namespace detail

// QTask<void> is rejected up front: WhenAnyAwaitable would form `const
// void &` deep inside its callbacks, which produces an impenetrable
// diagnostic. void and heterogeneous (variant) support are planned.
template<typename T, typename... Rest>
    requires(!std::is_void_v<T>) && (std::same_as<QTask<T>, std::remove_cvref_t<Rest>> && ...)
auto whenAny(QTask<T> & first, Rest &... rest) {
    return detail::WhenAnyAwaitable<T, 1 + sizeof...(Rest)>{{&first, &rest...}};
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
    // shared_ptr: callbacks capture it by value so a completion racing this
    // awaitable's destruction never posts to a freed context object.
    std::shared_ptr<QObject> context;

    CancelledByAwaitable(QTask<T> & t, std::stop_token tok)
        : task(t),
          token(std::move(tok)) {}

    ~CancelledByAwaitable() {
        if (guard)
            guard->store(true, std::memory_order_release);
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
        Q_ASSERT_X(QThread::currentThread()->eventDispatcher(), "co_await cancelledBy",
                   "co_await requires a running event loop on this thread");

        guard = std::make_shared<std::atomic<bool>>(false);
        context = std::make_shared<QObject>();

        auto g = guard;
        auto ctx = context;

        auto resume = [this, handle, g, ctx](bool isCancelled) {
            if (g->load(std::memory_order_acquire))
                return;
            QMetaObject::invokeMethod(
                ctx.get(),
                [this, handle, g, ctx, isCancelled]() mutable {
                    if (g->exchange(true, std::memory_order_acq_rel))
                        return;
                    cancelled = isCancelled;
                    handle.resume();
                },
                Qt::QueuedConnection);
        };

        if constexpr (std::is_void_v<T>) {
            task.then([resume]() { resume(false); });
        } else {
            task.then([resume](const T &) { resume(false); });
        }
        task.onCancelled([resume](const auto &) { resume(false); });
        task.onError([resume](auto) { resume(false); });

        if (token.stop_possible()) {
            stopCb = std::make_unique<StopCallback>(token, [resume]() { resume(true); });
        }
    }

    auto await_resume() -> decltype(task.await_resume()) {
        stopCb.reset();
        if (cancelled)
            throw utils::AwaitCancelled{utils::AwaitCancelled::Stopped};
        return task.await_resume();
    }
};

} // namespace detail

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
    // See CancelledByAwaitable::context for why this is a shared_ptr.
    std::shared_ptr<QObject> context;

    TaskTimeoutAwaitable(QTask<T> & t, std::chrono::milliseconds m)
        : task(t),
          ms(m) {}

    ~TaskTimeoutAwaitable() {
        if (timer)
            timer->stop();
        if (guard)
            guard->store(true, std::memory_order_release);
    }

    TaskTimeoutAwaitable(TaskTimeoutAwaitable &&) = default;
    TaskTimeoutAwaitable & operator=(TaskTimeoutAwaitable &&) = default;

    bool await_ready() {
        return task.await_ready();
    }

    void await_suspend(std::coroutine_handle<> handle) {
        Q_ASSERT_X(QThread::currentThread()->eventDispatcher(), "co_await withTimeout",
                   "co_await requires a running event loop on this thread");

        guard = std::make_shared<std::atomic<bool>>(false);
        context = std::make_shared<QObject>();

        auto g = guard;
        auto ctx = context;

        auto resume = [handle, g, ctx]() {
            if (g->load(std::memory_order_acquire))
                return;
            QMetaObject::invokeMethod(
                ctx.get(),
                [handle, g, ctx]() mutable {
                    if (g->exchange(true, std::memory_order_acq_rel))
                        return;
                    handle.resume();
                },
                Qt::QueuedConnection);
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
        QObject::connect(
            timer.get(), &QTimer::timeout, ctx.get(),
            [this, handle, g]() {
                if (g->exchange(true, std::memory_order_acq_rel))
                    return;
                timedOut = true;
                handle.resume();
            },
            Qt::QueuedConnection);
        timer->start(ms);
    }

    auto await_resume() -> decltype(task.await_resume()) {
        if (timedOut)
            throw utils::AwaitCancelled{utils::AwaitCancelled::Timeout};
        return task.await_resume();
    }
};

} // namespace detail

template<typename T>
QTask<T> withTimeout(QTask<T> & task, std::chrono::milliseconds ms) {
    co_return co_await detail::TaskTimeoutAwaitable<T>(task, ms);
}

inline QTask<void> withTimeout(QTask<void> & task, std::chrono::milliseconds ms) {
    co_await detail::TaskTimeoutAwaitable<void>(task, ms);
}

} // namespace QtCoroutine
