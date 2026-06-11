#pragma once
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <QMutex>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <stop_token>
#include <tuple>
#include <utility>
#include "qtcoroutine.hpp"
#include "utils.hpp"

namespace QtCoroutine {

template<typename... Args>
class QSignalStream;

namespace detail {

// next() result shape, mirroring ConnectResultT's arity dispatch but with
// stream termination encoded: 0 args -> bool (false = ended), 1 arg ->
// optional<T>, N args -> optional<tuple> (nullopt = ended).
template<typename... Args>
struct StreamNextResult {
    using type = std::optional<std::tuple<Args...>>;
};
template<>
struct StreamNextResult<> {
    using type = bool;
};
template<typename T>
struct StreamNextResult<T> {
    using type = std::optional<T>;
};

// Maps a signal's decayed argument tuple to the stream specialization, so
// stream() can deduce QSignalStream<int> from void (S::*)(int).
template<typename Tuple>
struct StreamFor;
template<typename... A>
struct StreamFor<std::tuple<A...>> {
    using type = QSignalStream<A...>;
};

// Qt declares some signals with a trailing QPrivateSignal argument to
// forbid external emission (e.g. QTimer::timeout). It is an implementation
// detail of the sender — strip it so QSignalStream<QTimer::QPrivateSignal>
// doesn't happen. Detection as in Qt's own qfuture_impl.h: the elaborated
// `class T::QPrivateSignal` finds the nested class without falling into
// the rules of [class.qual]/2.
template<typename T, typename = void>
inline constexpr bool isPrivateSignalArg = false;
template<typename T>
inline constexpr bool isPrivateSignalArg<T, std::enable_if_t<std::is_class_v<class T::QPrivateSignal>>> = true;

template<typename Tuple, typename Seq>
struct TupleSelect;
template<typename Tuple, std::size_t... I>
struct TupleSelect<Tuple, std::index_sequence<I...>> {
    using type = std::tuple<std::tuple_element_t<I, Tuple>...>;
};

template<typename Tuple>
struct StripPrivateSignal {
    using type = Tuple;
};
template<typename... A>
    requires(sizeof...(A) > 0) && isPrivateSignalArg<std::tuple_element_t<sizeof...(A) - 1, std::tuple<A...>>>
struct StripPrivateSignal<std::tuple<A...>>
    : TupleSelect<std::tuple<A...>, std::make_index_sequence<sizeof...(A) - 1>> {};

} // namespace detail

// ------------------------------------------------------------------
// QSignalStream – lossless co_await loop over a repeating Qt signal
// ------------------------------------------------------------------
//
// co_await signal(...) captures exactly one emission; re-arming in a loop
// loses emissions that arrive between resume and re-arm. A stream connects
// once, AT CONSTRUCTION (eager, like everything else here), queues every
// emission, and hands them out one per co_await:
//
//     auto bytes = QtCoroutine::stream(&device, &QIODevice::readyRead);
//     while (co_await bytes.next()) {           // 0-arg signal -> bool
//         process(device.readAll());
//     }
//
//     auto progress = QtCoroutine::stream(&worker, &Worker::progress);
//     while (auto p = co_await progress.next()) // 1-arg -> optional<int>
//         updateBar(*p);
//
// The template arguments are the signal's decayed argument types, not the
// sender — QSignalStream<int> is storable as a member without spelling a
// member-function-pointer type. Use stream() to deduce them.
//
// Termination: the sender being destroyed is normal lifecycle, not an
// error — remaining queued values still drain, then next() returns
// nullopt/false forever (EOF, like reading a file). Exceptions are
// reserved for caller-side aborts, consistent with the one-shot await:
//  - .cancelledBy(st) triggered -> AwaitCancelled{Stopped}, immediately
//    (no drain), terminal.
//  - .withTimeout(ms) -> a per-next INACTIVITY timeout: each next() that
//    actually has to wait throws AwaitCancelled{Timeout} after ms of
//    silence. Non-fatal: the stream stays armed and usable — a queued
//    value always wins over a timeout that raced it.
//  - .resumeOn(ctx) destroyed -> AwaitCancelled{ResumeContextDestroyed},
//    terminal.
//
// Buffering: unbounded by default (truly lossless, like Qt's own queued
// connections). .latestOnly() switches to depth-1 conflation — right for
// progress/state signals where stale intermediates are worthless.
//
// Thread semantics (same model as QSignalAwaitable):
//  - Emissions are inputs and unrestricted: any thread may emit; values
//    are enqueued directly from the emitting thread under the control
//    block's mutex, so nothing is lost even while the consumer is busy.
//  - next() resumes on the stream's consumer thread — the construction
//    thread by default, ctx's thread after .resumeOn(ctx). A same-thread
//    emission resumes a parked next() synchronously inside the emit
//    (direct-slot semantics); cross-thread emissions marshal the wake.
//  - The handle is thread-affine: next() must be awaited on the consumer
//    thread (debug-asserted). At most one next() may be pending at a time,
//    and the stream must outlive a pending next() (both debug-asserted) —
//    naturally satisfied by the consuming-loop pattern.
//
// Builders are rvalue-qualified, configured once at construction time:
//     auto s = stream(&w, &Worker::progress).latestOnly().cancelledBy(st);
// For value-based error handling wrap the await itself:
//     auto r = co_await s.next().asExpected();
template<typename... Args>
class [[nodiscard(
    "a discarded stream disconnects at the end of the statement — keep it and co_await next()")]] QSignalStream {
    using Tuple = std::tuple<Args...>;
    using NextResult = typename detail::StreamNextResult<Args...>::type;
    using StopCallback = std::stop_callback<std::function<void()>>;
    static constexpr std::size_t argCount = sizeof...(Args);

    // Shared between the handle, the connection lambdas, the stop callback
    // and pending timers/posted wakes. Callbacks only ever touch this, never
    // the handle or a next-awaitable. Everything below the mutex line is
    // guarded by it; transitions are serialized, so there is no lost-wakeup
    // window (await_suspend re-checks under the lock before parking).
    struct ControlBlock {
        QMutex mutex;
        std::deque<Tuple> queue;
        bool latestOnly = false;
        bool stopped = false;           // stop token fired — terminal, no drain
        bool ended = false;             // sender destroyed — EOF after drain
        bool ctxDestroyed = false;      // resumeOn() context destroyed — terminal
        std::coroutine_handle<> waiter; // parked next(), at most one
        std::uint64_t gen = 0;          // bumped per park; binds a timeout to its wait
        std::uint64_t timedOutGen = 0;  // gen whose wait timed out (0 = none)
        bool notifyPosted = false;      // coalesces cross-thread wake posts
        // Consumer-thread delivery target. Owned (fresh QObject) by default;
        // non-owning aliasing pointer after resumeOn(). shared_ptr so posted
        // wakes keep the default context alive until delivered.
        std::shared_ptr<QObject> context;
        QMetaObject::Connection signalConnection;
        QMetaObject::Connection senderDestroyedConnection;
        QMetaObject::Connection ctxDestroyedConnection;

        // Wake a parked next() if an input made it deliverable. Called by
        // the input paths with the mutex held (via `lock`); unlocks before
        // resuming so the consumer can re-enter next() freely.
        void notify(const std::shared_ptr<ControlBlock> & self, QMutexLocker<QMutex> & lock) {
            if (!waiter)
                return;
            if (!stopped && !ended && queue.empty())
                return;
            // After ctxDestroyed the waiter was already taken by that
            // handler; this also keeps us from touching a dangling context.
            if (ctxDestroyed)
                return;
            if (QThread::currentThread() == context->thread()) {
                // Same-thread emit: resume synchronously inside the emit,
                // exactly like a direct-connected slot.
                auto h = std::exchange(waiter, std::coroutine_handle<>{});
                lock.unlock();
                h.resume();
            } else {
                if (notifyPosted)
                    return;
                notifyPosted = true;
                auto ctx = context;
                QMetaObject::invokeMethod(
                    ctx.get(),
                    [self]() {
                        std::coroutine_handle<> h;
                        {
                            QMutexLocker lock(&self->mutex);
                            self->notifyPosted = false;
                            if (self->waiter && (self->stopped || self->ended || !self->queue.empty()))
                                h = std::exchange(self->waiter, std::coroutine_handle<>{});
                        }
                        if (h)
                            h.resume();
                    },
                    Qt::QueuedConnection);
            }
        }
    };

    // The per-co_await awaitable. Holds only the control block: if the
    // awaiting frame is destroyed while parked, the destructor (running in
    // the frame) unparks under the mutex, so a later emission never resumes
    // dead memory.
    class [[nodiscard]] NextAwaitable {
    public:
        explicit NextAwaitable(std::shared_ptr<ControlBlock> cb, std::optional<std::chrono::milliseconds> timeout)
            : m_cb(std::move(cb)),
              m_timeout(timeout) {}

        ~NextAwaitable() {
            if (!m_cb)
                return;
            QMutexLocker lock(&m_cb->mutex);
            if (m_cb->waiter && m_cb->gen == m_gen)
                m_cb->waiter = {};
        }

        NextAwaitable(const NextAwaitable &) = delete;
        NextAwaitable & operator=(const NextAwaitable &) = delete;
        NextAwaitable(NextAwaitable && other) noexcept
            : m_cb(std::move(other.m_cb)),
              m_timeout(other.m_timeout),
              m_gen(other.m_gen) {}

        [[nodiscard]] ExpectedAwaitable<NextAwaitable> asExpected() && {
            return ExpectedAwaitable<NextAwaitable>(std::move(*this));
        }

        bool await_ready() {
            QMutexLocker lock(&m_cb->mutex);
            return m_cb->stopped || m_cb->ctxDestroyed || m_cb->ended || !m_cb->queue.empty();
        }

        bool await_suspend(std::coroutine_handle<> handle) {
            auto & cb = *m_cb;
            Q_ASSERT_X(QThread::currentThread()->eventDispatcher(), "QSignalStream::next",
                       "co_await requires a running event loop on this thread");
            QMutexLocker lock(&cb.mutex);
            Q_ASSERT_X(QThread::currentThread() == cb.context->thread(), "QSignalStream::next",
                       "next() must be awaited on the stream's consumer thread");
            // Re-check in the gap since await_ready: an emission from
            // another thread may have landed.
            if (cb.stopped || cb.ctxDestroyed || cb.ended || !cb.queue.empty())
                return false;
            Q_ASSERT_X(!cb.waiter, "QSignalStream::next", "only one coroutine may await next() at a time");
            cb.waiter = handle;
            m_gen = ++cb.gen;
            if (m_timeout) {
                // Receiver-targeted: fires on the consumer thread; the gen
                // check drops it once this wait was satisfied. A stale fire
                // merely keeps the control block alive until expiry.
                QTimer::singleShot(*m_timeout, cb.context.get(), [cbp = m_cb, myGen = m_gen]() {
                    std::coroutine_handle<> h;
                    {
                        QMutexLocker lock(&cbp->mutex);
                        if (!cbp->waiter || cbp->gen != myGen)
                            return;
                        cbp->timedOutGen = myGen;
                        h = std::exchange(cbp->waiter, std::coroutine_handle<>{});
                    }
                    h.resume();
                });
            }
            return true;
        }

        // A queued value, or false/nullopt at end-of-stream — or throws
        // AwaitCancelled for the caller-side aborts. Order matters: stop
        // and context loss abort without draining; a value wins over a
        // timeout that raced it (the timeout is dropped — non-fatal).
        NextResult await_resume() {
            auto & cb = *m_cb;
            QMutexLocker lock(&cb.mutex);
            if (cb.stopped)
                throw utils::AwaitCancelled{utils::AwaitCancelled::Stopped};
            if (cb.ctxDestroyed)
                throw utils::AwaitCancelled{utils::AwaitCancelled::ResumeContextDestroyed};
            if (!cb.queue.empty()) {
                Tuple value = std::move(cb.queue.front());
                cb.queue.pop_front();
                if constexpr (argCount == 0)
                    return true;
                else if constexpr (argCount == 1)
                    return std::move(std::get<0>(value));
                else
                    return std::move(value);
            }
            if (cb.ended) {
                if constexpr (argCount == 0)
                    return false;
                else
                    return std::nullopt;
            }
            if (m_gen != 0 && cb.timedOutGen == m_gen)
                throw utils::AwaitCancelled{utils::AwaitCancelled::Timeout};
            // Every resume path establishes one of the conditions above
            // before waking, on the consumer thread, with no gap.
            std::unreachable();
        }

    private:
        std::shared_ptr<ControlBlock> m_cb;
        std::optional<std::chrono::milliseconds> m_timeout;
        std::uint64_t m_gen = 0; // 0 = never parked
    };

public:
    // Connects immediately — emissions from here on are queued. Prefer the
    // stream() factory, which deduces Args from the signal.
    template<typename Sender, typename Signal>
        requires std::derived_from<Sender, QObject>
    QSignalStream(Sender * sender, Signal sig)
        : m_cb(std::make_shared<ControlBlock>()) {
        m_cb->context = std::make_shared<QObject>();
        auto cb = m_cb;
        // No connection context: runs directly on the emitting thread, so
        // enqueueing never depends on any event loop being responsive.
        // Only the first argCount arguments are queued — a trailing
        // QPrivateSignal (stripped from Args by stream()) is dropped.
        m_cb->signalConnection = QObject::connect(sender, sig, [cb](auto &&... args) {
            QMutexLocker lock(&cb->mutex);
            if (cb->stopped || cb->ended || cb->ctxDestroyed)
                return;
            if (cb->latestOnly)
                cb->queue.clear();
            [&]<std::size_t... I>(std::index_sequence<I...>, auto && all) {
                cb->queue.emplace_back(std::get<I>(std::move(all))...);
            }(std::make_index_sequence<argCount>{}, std::forward_as_tuple(std::forward<decltype(args)>(args)...));
            cb->notify(cb, lock);
        });
        m_cb->senderDestroyedConnection = QObject::connect(sender, &QObject::destroyed, [cb]() {
            QMutexLocker lock(&cb->mutex);
            cb->ended = true;
            cb->notify(cb, lock);
        });
    }

    ~QSignalStream() {
        dispose();
    }

    QSignalStream(const QSignalStream &) = delete;
    QSignalStream & operator=(const QSignalStream &) = delete;

    QSignalStream(QSignalStream && other) noexcept = default;

    QSignalStream & operator=(QSignalStream && other) noexcept {
        if (this != &other) {
            dispose();
            m_cb = std::move(other.m_cb);
            m_stopCallback = std::move(other.m_stopCallback);
            m_timeout = other.m_timeout;
        }
        return *this;
    }

    // ---- Builder methods (rvalue-qualified for safe chaining) ----

    // Depth-1 conflation: each emission replaces anything still queued.
    [[nodiscard]] QSignalStream latestOnly() && {
        QMutexLocker lock(&m_cb->mutex);
        m_cb->latestOnly = true;
        if (m_cb->queue.size() > 1)
            m_cb->queue.erase(m_cb->queue.begin(), m_cb->queue.end() - 1);
        return std::move(*this);
    }

    // Terminal abort: a pending and every later next() throws
    // AwaitCancelled{Stopped} immediately — queued values are not drained.
    [[nodiscard]] QSignalStream cancelledBy(std::stop_token st) && {
        if (st.stop_possible()) {
            auto cb = m_cb;
            m_stopCallback = std::make_unique<StopCallback>(std::move(st), [cb]() {
                QMutexLocker lock(&cb->mutex);
                cb->stopped = true;
                cb->notify(cb, lock);
            });
        }
        return std::move(*this);
    }

    // Per-next inactivity timeout — see the class doc. Non-fatal.
    [[nodiscard]] QSignalStream withTimeout(std::chrono::milliseconds ms) && {
        m_timeout = ms;
        return std::move(*this);
    }

    // Migrates delivery: next() must then be awaited on ctx's thread, and
    // the consuming loop runs there. If ctx is destroyed while the stream
    // is in use, next() throws AwaitCancelled{ResumeContextDestroyed}.
    [[nodiscard]] QSignalStream resumeOn(QObject * ctx) && {
        auto cb = m_cb;
        QMutexLocker lock(&cb->mutex);
        cb->context = std::shared_ptr<QObject>(ctx, [](QObject *) {});
        // No connection context: runs directly on the thread destroying
        // ctx (normally the consumer thread, where the waiter parked).
        cb->ctxDestroyedConnection = QObject::connect(ctx, &QObject::destroyed, [cb]() {
            std::coroutine_handle<> h;
            {
                QMutexLocker lock(&cb->mutex);
                cb->ctxDestroyed = true;
                h = std::exchange(cb->waiter, std::coroutine_handle<>{});
            }
            if (h)
                h.resume();
        });
        return std::move(*this);
    }

    // ---- Consumption ----

    [[nodiscard]] NextAwaitable next() {
        Q_ASSERT_X(m_cb, "QSignalStream::next", "moved-from stream");
        return NextAwaitable(m_cb, m_timeout);
    }

private:
    void dispose() {
        if (!m_cb)
            return;
        // Outside the mutex: the stop callback locks it, and ~stop_callback
        // waits out a concurrently-running one.
        m_stopCallback.reset();
        QObject::disconnect(m_cb->signalConnection);
        QObject::disconnect(m_cb->senderDestroyedConnection);
        QObject::disconnect(m_cb->ctxDestroyedConnection);
        {
            // Scoped: releasing m_cb may destroy the control block — the
            // mutex must not be held (or destroyed locked) when it does.
            QMutexLocker lock(&m_cb->mutex);
            Q_ASSERT_X(!m_cb->waiter, "~QSignalStream",
                       "stream destroyed while a next() is pending — the stream must outlive its consumer loop");
        }
        m_cb.reset();
    }

    std::shared_ptr<ControlBlock> m_cb;
    std::unique_ptr<StopCallback> m_stopCallback;
    std::optional<std::chrono::milliseconds> m_timeout;
};

// ------------------------------------------------------------------
// stream() – factory, deduces QSignalStream<Args...> from the signal
// ------------------------------------------------------------------

template<typename Sender, typename Signal>
    requires std::derived_from<Sender, QObject>
[[nodiscard]] auto stream(Sender * sender, Signal sig) {
    using ArgsTuple = typename detail::StripPrivateSignal<typename utils::SignalArgs<Signal>::tuple_type>::type;
    using Stream = typename detail::StreamFor<ArgsTuple>::type;
    return Stream(sender, sig);
}

} // namespace QtCoroutine
