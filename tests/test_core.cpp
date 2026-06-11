#include <QCoreApplication>
#include <QTimer>
#include <QThread>
#include <QtConcurrent>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <QtCoroutine>

// ---- Test helper ----

static int g_passed = 0;
static int g_failed = 0;

#define TEST_ASSERT(expr, msg)                                                                                         \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            std::cerr << "  FAIL: " << msg << " (" << #expr << ")\n";                                                  \
            ++g_failed;                                                                                                \
        } else {                                                                                                       \
            ++g_passed;                                                                                                \
        }                                                                                                              \
    } while (0)

// ---- Emitter used by signal tests ----

class Emitter : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

signals:
    void voidSignal();
    void oneArg(int value);
    void twoArgs(bool success, const QString & message);
};

// ====================================================================
// 1. QTask basics
// ====================================================================

QtCoroutine::QTask<int> taskReturnsValue() {
    co_return 42;
}

QtCoroutine::QTask<void> taskReturnsVoid() {
    co_return;
}

QtCoroutine::QTask<int> taskChain() {
    auto inner = taskReturnsValue();
    auto val = co_await inner;
    co_return val + 8;
}

void test_qtask_basic() {
    std::cout << "test_qtask_basic\n";

    // QTask<int> returns value
    auto t1 = taskReturnsValue();
    TEST_ASSERT(t1.await_ready(), "eager task should be done immediately");

    // QTask<void>
    auto t2 = taskReturnsVoid();
    TEST_ASSERT(t2.await_ready(), "void task should be done immediately");

    // Chained co_await
    auto t3 = taskChain();
    TEST_ASSERT(t3.await_ready(), "chained task should be done (both sync)");
    TEST_ASSERT(t3.await_resume() == 50, "chained task result should be 50");
}

// ====================================================================
// 2. QTask move semantics
// ====================================================================

void test_qtask_move() {
    std::cout << "test_qtask_move\n";

    auto t1 = taskReturnsValue();
    auto t2 = std::move(t1);
    TEST_ASSERT(t2.await_ready(), "moved-to task should be ready");
    TEST_ASSERT(t2.await_resume() == 42, "moved-to task result correct");

    // Move assignment
    auto t3 = taskReturnsValue();
    t3 = taskChain();
    TEST_ASSERT(t3.await_ready(), "move-assigned task should be ready");
    TEST_ASSERT(t3.await_resume() == 50, "move-assigned task result correct");
}

// ====================================================================
// 3. QTask .then() continuation
// ====================================================================

void test_qtask_then() {
    std::cout << "test_qtask_then\n";

    // .then() on already-done task fires immediately
    int captured = 0;
    auto t = taskReturnsValue();
    t.then([&](const int & val) { captured = val; });
    TEST_ASSERT(captured == 42, ".then() on done task fires immediately");

    // .then() on void task
    bool voidCalled = false;
    auto tv = taskReturnsVoid();
    tv.then([&]() { voidCalled = true; });
    TEST_ASSERT(voidCalled, ".then() on done void task fires immediately");
}

// ====================================================================
// 4. Signal awaiting — void signal
// ====================================================================

QtCoroutine::QTask<void> awaitVoidSignal(Emitter * e) {
    co_await QtCoroutine::signal(e, &Emitter::voidSignal);
}

void test_signal_void(QCoreApplication & app) {
    std::cout << "test_signal_void\n";

    Emitter e;
    auto task = awaitVoidSignal(&e);
    TEST_ASSERT(!task.await_ready(), "should be suspended waiting for signal");

    // Emit after a short delay
    QTimer::singleShot(10, [&]() { emit e.voidSignal(); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after signal");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 5. Signal awaiting — one arg
// ====================================================================

QtCoroutine::QTask<int> awaitOneArg(Emitter * e) {
    auto val = co_await QtCoroutine::signal(e, &Emitter::oneArg);
    co_return val;
}

void test_signal_one_arg(QCoreApplication & app) {
    std::cout << "test_signal_one_arg\n";

    Emitter e;
    auto task = awaitOneArg(&e);

    QTimer::singleShot(10, [&]() { emit e.oneArg(99); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after signal");
        TEST_ASSERT(task.await_resume() == 99, "should capture signal arg");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 6. Signal awaiting — two args (structured binding)
// ====================================================================

QtCoroutine::QTask<std::pair<bool, QString>> awaitTwoArgs(Emitter * e) {
    auto [ok, msg] = co_await QtCoroutine::signal(e, &Emitter::twoArgs);
    co_return std::pair{ok, msg};
}

void test_signal_two_args(QCoreApplication & app) {
    std::cout << "test_signal_two_args\n";

    Emitter e;
    auto task = awaitTwoArgs(&e);

    QTimer::singleShot(10, [&]() { emit e.twoArgs(true, "hello"); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after signal");
        auto [ok, msg] = task.await_resume();
        TEST_ASSERT(ok == true, "first arg correct");
        TEST_ASSERT(msg == "hello", "second arg correct");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 7. Builder — cancelledBy
// ====================================================================

QtCoroutine::QTask<void> awaitWithCancel(Emitter * e, std::stop_token st) {
    co_await QtCoroutine::signal(e, &Emitter::voidSignal).cancelledBy(st);
}

void test_cancellation(QCoreApplication & app) {
    std::cout << "test_cancellation\n";

    Emitter e;
    std::stop_source ss;
    auto task = awaitWithCancel(&e, ss.get_token());

    QTimer::singleShot(10, [&]() { ss.request_stop(); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after cancel");
        TEST_ASSERT(task.isCancelled(), "should report cancelled");
        TEST_ASSERT(task.cancelReason() == QtCoroutine::utils::AwaitCancelled::Stopped, "reason should be Stopped");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 8. Builder — resumeOn
// ====================================================================

QtCoroutine::QTask<int> awaitWithResumeCtx(Emitter * e, QObject * ctx) {
    auto val = co_await QtCoroutine::signal(e, &Emitter::oneArg).resumeOn(ctx);
    co_return val;
}

void test_resume_on(QCoreApplication & app) {
    std::cout << "test_resume_on\n";

    Emitter e;
    QObject ctx; // resume context
    auto task = awaitWithResumeCtx(&e, &ctx);

    QTimer::singleShot(10, [&]() { emit e.oneArg(77); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done");
        TEST_ASSERT(task.await_resume() == 77, "value correct");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 9. Sender destroyed
// ====================================================================

QtCoroutine::QTask<void> awaitDestroyedSender(Emitter * e) {
    co_await QtCoroutine::signal(e, &Emitter::voidSignal);
}

void test_sender_destroyed(QCoreApplication & app) {
    std::cout << "test_sender_destroyed\n";

    auto * e = new Emitter;
    auto task = awaitDestroyedSender(e);

    QTimer::singleShot(10, [&]() { delete e; });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after sender destroyed");
        TEST_ASSERT(task.isCancelled(), "should report cancelled");
        TEST_ASSERT(task.cancelReason() == QtCoroutine::utils::AwaitCancelled::SenderDestroyed,
                    "reason should be SenderDestroyed");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 10. QFuture co_await
// ====================================================================

QFuture<int> futureCoroutine() {
    auto f = QtConcurrent::run([]() { return 123; });
    auto val = co_await f;
    co_return val + 1;
}

void test_qfuture_await(QCoreApplication & app) {
    std::cout << "test_qfuture_await\n";

    auto future = futureCoroutine();

    QTimer::singleShot(200, [&]() {
        TEST_ASSERT(future.isFinished(), "future should be finished");
        TEST_ASSERT(future.result() == 124, "future result correct");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 10b. QFuture chained/concurrent await — self-deadlock regression
// ====================================================================
//
// Regression for: operator co_await(QFuture) self-deadlocks under
// chained/concurrent awaits.
//
// Root cause: the QFuture awaiter resumes the coroutine SYNCHRONOUSLY
// from inside QFuture::then's continuation, so the coroutine's next
// co_await re-enters QFutureInterfaceBase::setContinuation while still
// nested inside the continuation machinery — a thread blocking on a lock
// it already holds further up its own stack. The synchronous resume
// fires from either delivery route:
//   (a) during the event-loop spin, when a continuation posted from a
//       worker is delivered and drives the next co_await; or
//   (b) during eager construction, when an awaited future finishes in
//       the race window between await_ready() and setContinuation(): the
//       continuation, whose context lives on the current thread, is then
//       invoked directly (synchronously) instead of being posted.
// Driving many chains concurrently on one event loop reproduces it.
//
// NOTE: the bug freezes the thread itself, so a QTimer watchdog is
// useless (it is delivered by the same wedged thread). The watchdog runs
// on a SEPARATE std::thread, armed BEFORE construction (route (b) wedges
// before app.exec() runs), and force-exits the process so a regression
// surfaces as a failure instead of hanging forever.

static std::atomic<int> g_chainsDone{0};

QFuture<int> chainedFutures(int depth) {
    int sum = 0;
    for (int i = 0; i < depth; ++i)
        sum += co_await QtConcurrent::run([]() { return 1; });
    co_return sum;
}

void test_qfuture_chained_concurrent(QCoreApplication & app) {
    std::cout << "test_qfuture_chained_concurrent\n";

    constexpr int N = 200;    // concurrent coroutines on one event loop
    constexpr int DEPTH = 12; // chained co_await QFuture per coroutine

    g_chainsDone.store(0, std::memory_order_relaxed);

    // Arm the watchdog FIRST: the deadlock can strike during construction
    // (route (b) above), before the event loop ever runs.
    std::atomic<bool> finished{false};
    std::thread watchdog([&finished]() {
        for (int i = 0; i < 300; ++i) { // ~30s ceiling
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (finished.load(std::memory_order_acquire))
                return;
        }
        std::fputs("  FAIL: chained QFuture coroutines deadlocked "
                   "(thread wedged)\n",
                   stderr);
        std::fflush(stderr);
        std::_Exit(1);
    });

    std::vector<QFuture<int>> running;
    running.reserve(N);
    for (int k = 0; k < N; ++k) {
        QFuture<int> f = chainedFutures(DEPTH);
        // Count completions on the event-loop thread; quit once all finish.
        f.then(&app, [&app](int v) {
            TEST_ASSERT(v == DEPTH, "each chain sums to DEPTH");
            if (g_chainsDone.fetch_add(1, std::memory_order_acq_rel) + 1 == N)
                app.quit();
        });
        running.push_back(std::move(f));
    }

    app.exec();
    finished.store(true, std::memory_order_release);
    watchdog.join();

    TEST_ASSERT(g_chainsDone.load() == N, "all chained QFuture coroutines completed (no deadlock)");
}

// ====================================================================
// 11. QTask<int> cancellation propagation
// ====================================================================

QtCoroutine::QTask<int> awaitOneArgWithCancel(Emitter * e, std::stop_token st) {
    auto val = co_await QtCoroutine::signal(e, &Emitter::oneArg).cancelledBy(st);
    co_return val;
}

void test_qtask_cancellation_state(QCoreApplication & app) {
    std::cout << "test_qtask_cancellation_state\n";

    Emitter e;
    std::stop_source ss;
    auto task = awaitOneArgWithCancel(&e, ss.get_token());

    QTimer::singleShot(10, [&]() { ss.request_stop(); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done");
        TEST_ASSERT(task.isCancelled(), "should be cancelled");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 12. readyIf — 0-arg signal (bool predicate)
// ====================================================================

QtCoroutine::QTask<void> awaitWithReadyCheckVoid(Emitter * e) {
    co_await QtCoroutine::signal(e, &Emitter::voidSignal).readyIf([](Emitter *) { return true; }); // already ready
}

void test_readyIf_void() {
    std::cout << "test_readyIf_void\n";

    Emitter e;
    auto task = awaitWithReadyCheckVoid(&e);
    TEST_ASSERT(task.await_ready(), "readyIf(true) should make task done immediately");
}

// ====================================================================
// 13. readyIf — 1-arg signal (returns std::optional<int>)
// ====================================================================

QtCoroutine::QTask<int> awaitWithReadyCheckOneArg(Emitter * e, bool ready) {
    auto val = co_await QtCoroutine::signal(e, &Emitter::oneArg).readyIf([ready](Emitter *) -> std::optional<int> {
        if (ready)
            return 999;
        return std::nullopt;
    });
    co_return val;
}

void test_readyIf_one_arg(QCoreApplication & app) {
    std::cout << "test_readyIf_one_arg\n";

    // (a) ready=true → task done immediately with value 999
    {
        Emitter e;
        auto task = awaitWithReadyCheckOneArg(&e, true);
        TEST_ASSERT(task.await_ready(), "readyIf(true) should be done immediately");
        TEST_ASSERT(task.await_resume() == 999, "readyIf(true) should return 999");
    }

    // (b) ready=false → task suspends, emit signal with 77
    {
        Emitter e;
        auto task = awaitWithReadyCheckOneArg(&e, false);
        TEST_ASSERT(!task.await_ready(), "readyIf(false) should suspend");

        QTimer::singleShot(10, [&]() { emit e.oneArg(77); });
        QTimer::singleShot(50, [&]() {
            TEST_ASSERT(task.await_ready(), "should be done after signal");
            TEST_ASSERT(task.await_resume() == 77, "should capture signal arg 77");
            app.quit();
        });

        app.exec();
    }
}

// ====================================================================
// 14. readyIf — 2-arg signal (returns std::optional<std::tuple>)
// ====================================================================

QtCoroutine::QTask<std::pair<bool, QString>> awaitWithReadyCheckTwoArgs(Emitter * e) {
    auto [ok, msg] = co_await QtCoroutine::signal(e, &Emitter::twoArgs)
                         .readyIf([](Emitter *) -> std::optional<std::tuple<bool, QString>> {
                             return std::tuple{true, QString("cached")};
                         });
    co_return std::pair{ok, msg};
}

void test_readyIf_two_args() {
    std::cout << "test_readyIf_two_args\n";

    Emitter e;
    auto task = awaitWithReadyCheckTwoArgs(&e);
    TEST_ASSERT(task.await_ready(), "readyIf with cached tuple should be done immediately");
    auto [ok, msg] = task.await_resume();
    TEST_ASSERT(ok == true, "cached first arg correct");
    TEST_ASSERT(msg == "cached", "cached second arg correct");
}

// ====================================================================
// 15. Exception propagation through QTask chain
// ====================================================================

QtCoroutine::QTask<int> taskThatThrows() {
    throw std::runtime_error("test error");
    co_return 0; // never reached
}

QtCoroutine::QTask<int> taskChainWithException() {
    auto val = co_await taskThatThrows();
    co_return val; // never reached
}

void test_exception_propagation() {
    std::cout << "test_exception_propagation\n";

    auto task = taskChainWithException();
    TEST_ASSERT(task.await_ready(), "task with exception should be done");

    bool caught = false;
    try {
        task.await_resume();
    } catch (const std::runtime_error & e) {
        caught = true;
        TEST_ASSERT(std::string(e.what()) == "test error", "exception message correct");
    }
    TEST_ASSERT(caught, "std::runtime_error should be rethrown");
}

// ====================================================================
// 16. QFuture<void> coroutine
// ====================================================================

QFuture<void> futureVoidCoroutine() {
    co_await QtConcurrent::run([]() { /* noop */ });
    co_return;
}

void test_qfuture_void(QCoreApplication & app) {
    std::cout << "test_qfuture_void\n";

    auto future = futureVoidCoroutine();

    QTimer::singleShot(200, [&]() {
        TEST_ASSERT(future.isFinished(), "void future should be finished");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 17. sleep()
// ====================================================================

QtCoroutine::QTask<int> sleepAndReturn() {
    co_await QtCoroutine::sleep(std::chrono::milliseconds(50));
    co_return 7;
}

void test_sleep(QCoreApplication & app) {
    std::cout << "test_sleep\n";

    auto task = sleepAndReturn();
    TEST_ASSERT(!task.await_ready(), "sleep task should NOT be done immediately");

    QTimer::singleShot(100, [&]() {
        TEST_ASSERT(task.await_ready(), "sleep task should be done after 100ms");
        TEST_ASSERT(task.await_resume() == 7, "sleep task result should be 7");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 18. withTimeout — signal arrives before timeout
// ====================================================================

QtCoroutine::QTask<int> awaitWithTimeoutSuccess(Emitter * e) {
    auto val = co_await QtCoroutine::signal(e, &Emitter::oneArg).withTimeout(std::chrono::milliseconds(500));
    co_return val;
}

void test_withTimeout_success(QCoreApplication & app) {
    std::cout << "test_withTimeout_success\n";

    Emitter e;
    auto task = awaitWithTimeoutSuccess(&e);

    QTimer::singleShot(10, [&]() { emit e.oneArg(42); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after signal");
        TEST_ASSERT(task.await_resume() == 42, "should capture signal value");
        TEST_ASSERT(!task.isCancelled(), "should NOT be cancelled");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 19. withTimeout — timeout fires before signal
// ====================================================================

QtCoroutine::QTask<void> awaitWithTimeoutExpired(Emitter * e) {
    co_await QtCoroutine::signal(e, &Emitter::voidSignal).withTimeout(std::chrono::milliseconds(20));
}

void test_withTimeout_expired(QCoreApplication & app) {
    std::cout << "test_withTimeout_expired\n";

    Emitter e;
    auto task = awaitWithTimeoutExpired(&e);

    QTimer::singleShot(100, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after timeout");
        TEST_ASSERT(task.isCancelled(), "should report cancelled");
        TEST_ASSERT(task.cancelReason() == QtCoroutine::utils::AwaitCancelled::Timeout, "reason should be Timeout");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 20. QTask<void> move-assignment
// ====================================================================

void test_qtask_void_move_assignment() {
    std::cout << "test_qtask_void_move_assignment\n";

    auto t1 = taskReturnsVoid();
    auto t2 = taskReturnsVoid();
    t1 = std::move(t2);
    TEST_ASSERT(t1.await_ready(), "move-assigned void task should be ready");
}

// ====================================================================
// 21. asExpected — success (signal arrives, returns value)
// ====================================================================

QtCoroutine::QTask<std::expected<int, QtCoroutine::utils::AwaitCancelled>> awaitAsExpectedSuccess(Emitter * e) {
    auto result = co_await QtCoroutine::signal(e, &Emitter::oneArg).asExpected();
    co_return result;
}

void test_asExpected_success(QCoreApplication & app) {
    std::cout << "test_asExpected_success\n";

    Emitter e;
    auto task = awaitAsExpectedSuccess(&e);

    QTimer::singleShot(10, [&]() { emit e.oneArg(42); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after signal");
        auto result = task.await_resume();
        TEST_ASSERT(result.has_value(), "result should have value");
        TEST_ASSERT(*result == 42, "result value should be 42");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 22. asExpected — cancellation (returns unexpected)
// ====================================================================

QtCoroutine::QTask<std::expected<int, QtCoroutine::utils::AwaitCancelled>>
awaitAsExpectedCancelled(Emitter * e, std::stop_token st) {
    auto result = co_await QtCoroutine::signal(e, &Emitter::oneArg).cancelledBy(st).asExpected();
    co_return result;
}

void test_asExpected_cancelled(QCoreApplication & app) {
    std::cout << "test_asExpected_cancelled\n";

    Emitter e;
    std::stop_source ss;
    auto task = awaitAsExpectedCancelled(&e, ss.get_token());

    QTimer::singleShot(10, [&]() { ss.request_stop(); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after cancel");
        auto result = task.await_resume();
        TEST_ASSERT(!result.has_value(), "result should not have value");
        TEST_ASSERT(result.error().wasStopped(), "error reason should be stopped");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 23. asExpected — void signal success
// ====================================================================

QtCoroutine::QTask<std::expected<void, QtCoroutine::utils::AwaitCancelled>> awaitAsExpectedVoid(Emitter * e) {
    auto result = co_await QtCoroutine::signal(e, &Emitter::voidSignal).asExpected();
    co_return result;
}

void test_asExpected_void(QCoreApplication & app) {
    std::cout << "test_asExpected_void\n";

    Emitter e;
    auto task = awaitAsExpectedVoid(&e);

    QTimer::singleShot(10, [&]() { emit e.voidSignal(); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after signal");
        auto result = task.await_resume();
        TEST_ASSERT(result.has_value(), "result should have value");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 24. onCancelled — fires on cancellation (async)
// ====================================================================

QtCoroutine::QTask<int> awaitOneArgCancellable24(Emitter * e, std::stop_token st) {
    auto val = co_await QtCoroutine::signal(e, &Emitter::oneArg).cancelledBy(st);
    co_return val;
}

void test_onCancelled_fires(QCoreApplication & app) {
    std::cout << "test_onCancelled_fires\n";

    Emitter e;
    std::stop_source ss;
    auto task = awaitOneArgCancellable24(&e, ss.get_token());

    bool callbackFired = false;
    QtCoroutine::utils::AwaitCancelled::Reason receivedReason{};
    task.onCancelled([&](const QtCoroutine::utils::AwaitCancelled & c) {
        callbackFired = true;
        receivedReason = c.reason;
    });

    QTimer::singleShot(10, [&]() { ss.request_stop(); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(callbackFired, "onCancelled callback should have fired");
        TEST_ASSERT(receivedReason == QtCoroutine::utils::AwaitCancelled::Stopped,
                    "onCancelled reason should be Stopped");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 25. onCancelled — fires immediately if already cancelled (async)
// ====================================================================

QtCoroutine::QTask<int> awaitOneArgCancellable25(Emitter * e, std::stop_token st) {
    auto val = co_await QtCoroutine::signal(e, &Emitter::oneArg).cancelledBy(st);
    co_return val;
}

void test_onCancelled_fires_immediately(QCoreApplication & app) {
    std::cout << "test_onCancelled_fires_immediately\n";

    Emitter e;
    std::stop_source ss;
    auto task = awaitOneArgCancellable25(&e, ss.get_token());

    QTimer::singleShot(10, [&]() { ss.request_stop(); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "task should be done before registering callback");

        bool callbackFired = false;
        QtCoroutine::utils::AwaitCancelled::Reason receivedReason{};
        task.onCancelled([&](const QtCoroutine::utils::AwaitCancelled & c) {
            callbackFired = true;
            receivedReason = c.reason;
        });

        TEST_ASSERT(callbackFired, "onCancelled should fire immediately on already-cancelled task");
        TEST_ASSERT(receivedReason == QtCoroutine::utils::AwaitCancelled::Stopped,
                    "onCancelled reason should be Stopped");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 26. onError — fires on exception (sync)
// ====================================================================

void test_onError_fires() {
    std::cout << "test_onError_fires\n";

    auto task = taskThatThrows();

    bool callbackFired = false;
    std::string errorMsg;
    task.onError([&](std::exception_ptr ep) {
        callbackFired = true;
        try {
            std::rethrow_exception(ep);
        } catch (const std::runtime_error & ex) {
            errorMsg = ex.what();
        }
    });

    TEST_ASSERT(callbackFired, "onError callback should have fired");
    TEST_ASSERT(errorMsg == "test error", "onError should receive correct exception message");
}

// ====================================================================
// 27. onCancelled on QTask<void> (async)
// ====================================================================

QtCoroutine::QTask<void> awaitVoidCancellable27(Emitter * e, std::stop_token st) {
    co_await QtCoroutine::signal(e, &Emitter::voidSignal).cancelledBy(st);
}

void test_onCancelled_void_task(QCoreApplication & app) {
    std::cout << "test_onCancelled_void_task\n";

    Emitter e;
    std::stop_source ss;
    auto task = awaitVoidCancellable27(&e, ss.get_token());

    bool callbackFired = false;
    QtCoroutine::utils::AwaitCancelled::Reason receivedReason{};
    task.onCancelled([&](const QtCoroutine::utils::AwaitCancelled & c) {
        callbackFired = true;
        receivedReason = c.reason;
    });

    QTimer::singleShot(10, [&]() { ss.request_stop(); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(callbackFired, "onCancelled callback should have fired on void task");
        TEST_ASSERT(receivedReason == QtCoroutine::utils::AwaitCancelled::Stopped,
                    "onCancelled reason should be Stopped on void task");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 28. toFuture — success (async)
// ====================================================================

QtCoroutine::QTask<int> taskReturns42() {
    co_return 42;
}

void test_toFuture_success(QCoreApplication & app) {
    std::cout << "test_toFuture_success\n";

    auto task = taskReturns42();
    QFuture<int> future = task.toFuture();

    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(future.isFinished(), "toFuture should be finished");
        TEST_ASSERT(future.result() == 42, "toFuture result should be 42");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 29. toFuture — cancellation becomes exception (async)
// ====================================================================

QtCoroutine::QTask<int> awaitOneArgCancellable29(Emitter * e, std::stop_token st) {
    auto val = co_await QtCoroutine::signal(e, &Emitter::oneArg).cancelledBy(st);
    co_return val;
}

void test_toFuture_cancellation(QCoreApplication & app) {
    std::cout << "test_toFuture_cancellation\n";

    Emitter e;
    std::stop_source ss;
    auto task = awaitOneArgCancellable29(&e, ss.get_token());
    QFuture<int> future = task.toFuture();

    QTimer::singleShot(10, [&]() { ss.request_stop(); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(future.isFinished(), "toFuture should be finished after cancel");

        bool caught = false;
        try {
            future.result();
        } catch (...) {
            caught = true;
        }
        TEST_ASSERT(caught, "toFuture result() should throw on cancellation");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 30. toFuture — void task (async)
// ====================================================================

QtCoroutine::QTask<void> taskReturnsVoid30() {
    co_return;
}

void test_toFuture_void(QCoreApplication & app) {
    std::cout << "test_toFuture_void\n";

    auto task = taskReturnsVoid30();
    QFuture<void> future = task.toFuture();

    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(future.isFinished(), "toFuture<void> should be finished");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 31. whenAll — sync tasks (sync)
// ====================================================================

void test_whenAll_sync() {
    std::cout << "test_whenAll_sync\n";

    auto t1 = taskReturnsValue(); // 42
    auto t2 = taskReturns42();    // 42
    auto combined = QtCoroutine::whenAll(t1, t2);
    TEST_ASSERT(combined.await_ready(), "whenAll of sync tasks should be ready");
    auto [r1, r2] = combined.await_resume();
    TEST_ASSERT(r1 == 42, "first result correct");
    TEST_ASSERT(r2 == 42, "second result correct");
}

// ====================================================================
// 32. whenAll — async tasks (async)
// ====================================================================

QtCoroutine::QTask<int> awaitOneArg32a(Emitter * e) {
    co_return co_await QtCoroutine::signal(e, &Emitter::oneArg);
}

QtCoroutine::QTask<int> awaitOneArg32b(Emitter * e) {
    co_return co_await QtCoroutine::signal(e, &Emitter::oneArg);
}

QtCoroutine::QTask<std::tuple<int, int>> whenAllAsync(Emitter * e1, Emitter * e2) {
    auto t1 = awaitOneArg32a(e1);
    auto t2 = awaitOneArg32b(e2);
    co_return co_await QtCoroutine::whenAll(t1, t2);
}

void test_whenAll_async(QCoreApplication & app) {
    std::cout << "test_whenAll_async\n";

    Emitter e1, e2;
    auto task = whenAllAsync(&e1, &e2);

    QTimer::singleShot(10, [&]() { emit e1.oneArg(11); });
    QTimer::singleShot(20, [&]() { emit e2.oneArg(22); });
    QTimer::singleShot(100, [&]() {
        TEST_ASSERT(task.await_ready(), "whenAll should be done");
        auto [r1, r2] = task.await_resume();
        TEST_ASSERT(r1 == 11, "first async result correct");
        TEST_ASSERT(r2 == 22, "second async result correct");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 33. whenAll — void tasks (sync)
// ====================================================================

void test_whenAll_void() {
    std::cout << "test_whenAll_void\n";

    auto t1 = taskReturnsVoid();
    auto t2 = taskReturnsVoid();
    auto combined = QtCoroutine::whenAll(t1, t2);
    TEST_ASSERT(combined.await_ready(), "whenAll<void> should be ready");
}

// ====================================================================
// 34. whenAny — first task wins (async)
// ====================================================================

QtCoroutine::QTask<int> awaitOneArg34a(Emitter * e) {
    co_return co_await QtCoroutine::signal(e, &Emitter::oneArg);
}

QtCoroutine::QTask<int> awaitOneArg34b(Emitter * e) {
    co_return co_await QtCoroutine::signal(e, &Emitter::oneArg);
}

QtCoroutine::QTask<QtCoroutine::WhenAnyResult<int>> whenAnyTest34(Emitter * e1, Emitter * e2) {
    auto t1 = awaitOneArg34a(e1);
    auto t2 = awaitOneArg34b(e2);
    co_return co_await QtCoroutine::whenAny(t1, t2);
}

void test_whenAny_first_wins(QCoreApplication & app) {
    std::cout << "test_whenAny_first_wins\n";

    Emitter e1, e2;
    auto task = whenAnyTest34(&e1, &e2);

    // Only emit on e2 — it should win
    QTimer::singleShot(10, [&]() { emit e2.oneArg(55); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "whenAny should be done");
        auto [index, value] = task.await_resume();
        TEST_ASSERT(value == 55, "winning task result correct");
        TEST_ASSERT(index == 1, "winner index is the argument position");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 35. whenAny — already-done task wins immediately (sync)
// ====================================================================

void test_whenAny_immediate() {
    std::cout << "test_whenAny_immediate\n";

    auto t1 = taskReturnsValue(); // already done, 42
    auto t2 = taskReturns42();    // already done, 42
    auto awaitable = QtCoroutine::whenAny(t1, t2);
    TEST_ASSERT(awaitable.await_ready(), "whenAny should be immediately ready");
    auto [index, value] = awaitable.await_resume();
    TEST_ASSERT(value == 42, "result should be 42");
    TEST_ASSERT(index == 0, "first already-done task wins the ready check");
}

// ====================================================================
// 36. QTask destroyed while suspended (async)
// ====================================================================

QtCoroutine::QTask<void> awaitVoidSignal36(Emitter * e) {
    co_await QtCoroutine::signal(e, &Emitter::voidSignal);
}

void test_qtask_destroyed_while_suspended(QCoreApplication & app) {
    std::cout << "test_qtask_destroyed_while_suspended\n";

    Emitter e;
    {
        auto task = awaitVoidSignal36(&e);
        TEST_ASSERT(!task.await_ready(), "should be suspended");
        // task destroyed here while coroutine is suspended
    }

    // Emit signal into the void — connections should have been cleaned up
    // by QSignalAwaitable destructor. No crash.
    emit e.voidSignal();

    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(true, "no crash after emitting to destroyed task");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 37. Resume context destroyed mid-suspension (async)
// ====================================================================

QtCoroutine::QTask<void> awaitWithResumeCtx37(Emitter * e, QObject * ctx) {
    co_await QtCoroutine::signal(e, &Emitter::voidSignal).resumeOn(ctx);
}

void test_resume_context_destroyed(QCoreApplication & app) {
    std::cout << "test_resume_context_destroyed\n";

    Emitter e;
    auto * ctx = new QObject;
    auto task = awaitWithResumeCtx37(&e, ctx);

    QTimer::singleShot(10, [&]() { delete ctx; });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after ctx destroyed");
        TEST_ASSERT(task.isCancelled(), "should report cancelled");
        TEST_ASSERT(task.cancelReason() == QtCoroutine::utils::AwaitCancelled::ResumeContextDestroyed,
                    "reason should be ResumeContextDestroyed");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 38. Signal emitted before co_await (async)
// ====================================================================

void test_signal_before_coawait(QCoreApplication & app) {
    std::cout << "test_signal_before_coawait\n";

    Emitter e;
    emit e.oneArg(99); // Emitted BEFORE task is created

    auto task = awaitOneArg(&e); // Uses existing helper
    TEST_ASSERT(!task.await_ready(), "should NOT capture pre-emission signal");

    QTimer::singleShot(10, [&]() { emit e.oneArg(42); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after new signal");
        TEST_ASSERT(task.await_resume() == 42, "should get 42, not 99");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 39. Multiple rapid emissions — only first captured (async)
// ====================================================================

void test_multiple_emissions(QCoreApplication & app) {
    std::cout << "test_multiple_emissions\n";

    Emitter e;
    auto task = awaitOneArg(&e);

    QTimer::singleShot(10, [&]() {
        emit e.oneArg(10);
        emit e.oneArg(20);
        emit e.oneArg(30);
    });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done");
        TEST_ASSERT(task.await_resume() == 10, "should capture only first emission");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 40. whenAll — one task cancelled (async)
// ====================================================================

QtCoroutine::QTask<int> awaitOneArgCancellable40(Emitter * e, std::stop_token st) {
    co_return co_await QtCoroutine::signal(e, &Emitter::oneArg).cancelledBy(st);
}

QtCoroutine::QTask<std::tuple<int, int>> whenAllOneCancelled(Emitter * e1, Emitter * e2, std::stop_token st) {
    auto t1 = awaitOneArgCancellable40(e1, st);
    auto t2 = awaitOneArg32b(e2); // Reuse existing helper
    co_return co_await QtCoroutine::whenAll(t1, t2);
}

void test_whenAll_one_cancelled(QCoreApplication & app) {
    std::cout << "test_whenAll_one_cancelled\n";

    Emitter e1, e2;
    std::stop_source ss;
    auto task = whenAllOneCancelled(&e1, &e2, ss.get_token());

    QTimer::singleShot(10, [&]() { ss.request_stop(); });
    QTimer::singleShot(20, [&]() { emit e2.oneArg(22); });
    QTimer::singleShot(100, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done");
        TEST_ASSERT(task.isCancelled(), "whenAll should propagate cancellation");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 41. toFuture — exception propagation (async)
// ====================================================================

void test_toFuture_exception(QCoreApplication & app) {
    std::cout << "test_toFuture_exception\n";

    auto task = taskThatThrows();
    QFuture<int> future = task.toFuture();

    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(future.isFinished(), "future should be finished");
        bool caught = false;
        try {
            future.result();
        } catch (...) {
            caught = true;
        }
        TEST_ASSERT(caught, "future.result() should throw on errored task");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 42. withTimeout + cancelledBy together (async)
// ====================================================================

QtCoroutine::QTask<void> awaitWithTimeoutAndCancel(Emitter * e, std::stop_token st) {
    co_await QtCoroutine::signal(e, &Emitter::voidSignal).withTimeout(std::chrono::milliseconds(500)).cancelledBy(st);
}

void test_timeout_and_cancel_together(QCoreApplication & app) {
    std::cout << "test_timeout_and_cancel_together\n";

    Emitter e;
    std::stop_source ss;
    auto task = awaitWithTimeoutAndCancel(&e, ss.get_token());

    QTimer::singleShot(10, [&]() { ss.request_stop(); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done");
        TEST_ASSERT(task.isCancelled(), "should report cancelled");
        TEST_ASSERT(task.cancelReason() == QtCoroutine::utils::AwaitCancelled::Stopped,
                    "stop should win over timeout (it fired first)");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 38. QTask<std::expected<void, E>> — co_return {} for void expected
// ====================================================================

struct TestError {
    int code;
};

QtCoroutine::QTask<std::expected<void, TestError>> taskExpectedVoidSuccess() {
    co_return {}; // default-constructs expected (success) — co_return; would NOT compile
}

QtCoroutine::QTask<std::expected<void, TestError>> taskExpectedVoidError() {
    co_return std::unexpected{TestError{42}};
}

void test_qtask_expected_void() {
    std::cout << "test_qtask_expected_void\n";

    auto t1 = taskExpectedVoidSuccess();
    TEST_ASSERT(t1.await_ready(), "expected<void> success task should be done");
    auto r1 = t1.await_resume();
    TEST_ASSERT(r1.has_value(), "expected<void> success should have value");

    auto t2 = taskExpectedVoidError();
    TEST_ASSERT(t2.await_ready(), "expected<void> error task should be done");
    auto r2 = t2.await_resume();
    TEST_ASSERT(!r2.has_value(), "expected<void> error should not have value");
    TEST_ASSERT(r2.error().code == 42, "expected<void> error code should be 42");
}

// ====================================================================
// 46. cancelledBy — task completes before stop (success) (async)
// ====================================================================

QtCoroutine::QTask<int> cancelledBySuccess46(Emitter * e, std::stop_token st) {
    auto task = awaitOneArg(e);
    co_return co_await QtCoroutine::cancelledBy(task, st);
}

void test_cancelledBy_success(QCoreApplication & app) {
    std::cout << "test_cancelledBy_success\n";

    Emitter e;
    std::stop_source ss;
    auto task = cancelledBySuccess46(&e, ss.get_token());

    QTimer::singleShot(10, [&]() { emit e.oneArg(42); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after signal");
        TEST_ASSERT(task.await_resume() == 42, "should return signal value");
        TEST_ASSERT(!task.isCancelled(), "should NOT be cancelled");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 47. cancelledBy — stop fires before task completes (async)
// ====================================================================

QtCoroutine::QTask<int> cancelledByStopped47(Emitter * e, std::stop_token st) {
    auto task = awaitOneArg(e);
    co_return co_await QtCoroutine::cancelledBy(task, st);
}

void test_cancelledBy_stopped(QCoreApplication & app) {
    std::cout << "test_cancelledBy_stopped\n";

    Emitter e;
    std::stop_source ss;
    auto task = cancelledByStopped47(&e, ss.get_token());

    QTimer::singleShot(10, [&]() { ss.request_stop(); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after stop");
        TEST_ASSERT(task.isCancelled(), "should report cancelled");
        TEST_ASSERT(task.cancelReason() == QtCoroutine::utils::AwaitCancelled::Stopped, "reason should be Stopped");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 48. cancelledBy + asExpected composition (async)
// ====================================================================

QtCoroutine::QTask<std::expected<int, QtCoroutine::utils::AwaitCancelled>> cancelledByExpected48(Emitter * e,
                                                                                                 std::stop_token st) {
    auto task = awaitOneArg(e);
    auto wrapped = QtCoroutine::cancelledBy(task, st);
    co_return co_await QtCoroutine::ExpectedAwaitable<QtCoroutine::QTask<int>>(std::move(wrapped));
}

void test_cancelledBy_asExpected(QCoreApplication & app) {
    std::cout << "test_cancelledBy_asExpected\n";

    Emitter e;
    std::stop_source ss;
    auto task = cancelledByExpected48(&e, ss.get_token());

    QTimer::singleShot(10, [&]() { ss.request_stop(); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after stop");
        auto result = task.await_resume();
        TEST_ASSERT(!result.has_value(), "should be unexpected (cancelled)");
        TEST_ASSERT(result.error().wasStopped(), "error reason should be stopped");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 43. whenAll — one task errors, waits for all before propagating (async)
// ====================================================================

QtCoroutine::QTask<int> awaitThenThrow43(Emitter * e) {
    co_await QtCoroutine::signal(e, &Emitter::voidSignal);
    throw std::runtime_error("whenAll error");
    co_return 0;
}

QtCoroutine::QTask<std::tuple<int, int>> whenAllOneErrors43(Emitter * eThrow, Emitter * eNormal) {
    auto t1 = awaitThenThrow43(eThrow);
    auto t2 = awaitOneArg32b(eNormal);
    co_return co_await QtCoroutine::whenAll(t1, t2);
}

void test_whenAll_one_errors(QCoreApplication & app) {
    std::cout << "test_whenAll_one_errors\n";

    Emitter eThrow, eNormal;
    auto task = whenAllOneErrors43(&eThrow, &eNormal);

    // t1 errors at 10ms, t2 completes at 30ms
    QTimer::singleShot(10, [&]() { emit eThrow.voidSignal(); });
    QTimer::singleShot(30, [&]() { emit eNormal.oneArg(22); });

    // At 20ms, t1 has errored but t2 hasn't completed — task should NOT be done
    QTimer::singleShot(
        20, [&]() { TEST_ASSERT(!task.await_ready(), "whenAll should wait for ALL tasks even when one errors"); });

    QTimer::singleShot(100, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after all tasks complete");
        bool caught = false;
        try {
            task.await_resume();
        } catch (const std::runtime_error & ex) {
            caught = true;
            TEST_ASSERT(std::string(ex.what()) == "whenAll error", "should propagate error from errored task");
        }
        TEST_ASSERT(caught, "whenAll should propagate error after all complete");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 44. whenAll — mixed async timing, results in declaration order (async)
// ====================================================================

QtCoroutine::QTask<std::tuple<int, int, int>> whenAllMixedTiming44(Emitter * e1, Emitter * e2, Emitter * e3) {
    auto t1 = awaitOneArg32b(e1);
    auto t2 = awaitOneArg32b(e2);
    auto t3 = awaitOneArg32b(e3);
    co_return co_await QtCoroutine::whenAll(t1, t2, t3);
}

void test_whenAll_mixed_timing(QCoreApplication & app) {
    std::cout << "test_whenAll_mixed_timing\n";

    Emitter e1, e2, e3;
    auto task = whenAllMixedTiming44(&e1, &e2, &e3);

    // Complete tasks in reverse order of declaration
    QTimer::singleShot(10, [&]() { emit e3.oneArg(33); });
    QTimer::singleShot(20, [&]() { emit e1.oneArg(11); });
    QTimer::singleShot(30, [&]() { emit e2.oneArg(22); });

    QTimer::singleShot(100, [&]() {
        TEST_ASSERT(task.await_ready(), "whenAll should be done after all complete");
        auto [r1, r2, r3] = task.await_resume();
        TEST_ASSERT(r1 == 11, "first result correct despite completing second");
        TEST_ASSERT(r2 == 22, "second result correct despite completing last");
        TEST_ASSERT(r3 == 33, "third result correct despite completing first");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 45. tryAll — short-circuits on first error (async)
// ====================================================================

QtCoroutine::QTask<std::tuple<int, int>> tryAllShortCircuit45(Emitter * eThrow, Emitter * eNormal) {
    auto t1 = awaitThenThrow43(eThrow);
    auto t2 = awaitOneArg32b(eNormal);
    co_return co_await QtCoroutine::tryAll(t1, t2);
}

void test_tryAll_short_circuits(QCoreApplication & app) {
    std::cout << "test_tryAll_short_circuits\n";

    Emitter eThrow, eNormal;
    auto task = tryAllShortCircuit45(&eThrow, &eNormal);

    // t1 errors at 10ms, t2 never completes — tryAll should still finish
    QTimer::singleShot(10, [&]() { emit eThrow.voidSignal(); });
    QTimer::singleShot(100, [&]() {
        TEST_ASSERT(task.await_ready(), "tryAll should be done after first error");
        bool caught = false;
        try {
            task.await_resume();
        } catch (const std::runtime_error & ex) {
            caught = true;
            TEST_ASSERT(std::string(ex.what()) == "whenAll error", "should propagate error from first failing task");
        }
        TEST_ASSERT(caught, "tryAll should short-circuit on first error");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 49. withTimeout (QTask) — task completes before timeout
// ====================================================================

QtCoroutine::QTask<int> withTimeoutSuccess49(Emitter * e) {
    auto inner = awaitOneArg(e);
    co_return co_await QtCoroutine::withTimeout(inner, std::chrono::milliseconds(500));
}

void test_withTimeout_qtask_success(QCoreApplication & app) {
    std::cout << "test_withTimeout_qtask_success\n";

    Emitter e;
    auto task = withTimeoutSuccess49(&e);

    QTimer::singleShot(10, [&]() { emit e.oneArg(42); });
    QTimer::singleShot(50, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after signal");
        TEST_ASSERT(task.await_resume() == 42, "should capture signal value");
        TEST_ASSERT(!task.isCancelled(), "should NOT be cancelled");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 50. withTimeout (QTask) — timeout fires before task completes
// ====================================================================

QtCoroutine::QTask<void> withTimeoutExpired50(Emitter * e) {
    auto inner = awaitVoidSignal(e);
    co_await QtCoroutine::withTimeout(inner, std::chrono::milliseconds(20));
}

void test_withTimeout_qtask_expired(QCoreApplication & app) {
    std::cout << "test_withTimeout_qtask_expired\n";

    Emitter e;
    auto task = withTimeoutExpired50(&e);

    QTimer::singleShot(100, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after timeout");
        TEST_ASSERT(task.isCancelled(), "should report cancelled");
        TEST_ASSERT(task.cancelReason() == QtCoroutine::utils::AwaitCancelled::Timeout, "reason should be Timeout");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 51. withTimeout (QTask) + ExpectedAwaitable composition
// ====================================================================

QtCoroutine::QTask<std::expected<int, QtCoroutine::utils::AwaitCancelled>> withTimeoutExpected51(Emitter * e) {
    auto inner = awaitOneArg(e);
    auto task = QtCoroutine::withTimeout(inner, std::chrono::milliseconds(20));
    co_return co_await QtCoroutine::ExpectedAwaitable<QtCoroutine::QTask<int>>(std::move(task));
}

void test_withTimeout_qtask_expected(QCoreApplication & app) {
    std::cout << "test_withTimeout_qtask_expected\n";

    Emitter e;
    auto task = withTimeoutExpected51(&e);

    QTimer::singleShot(100, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after timeout");
        auto result = task.await_resume();
        TEST_ASSERT(!result.has_value(), "result should not have value (timed out)");
        TEST_ASSERT(result.error().wasTimedOut(), "error should be timeout");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 52. withTimeout + QFuture-backed QTask, completes before timeout
// ====================================================================

QtCoroutine::QTask<int> taskFromQFuture() {
    co_return co_await QtConcurrent::run([]() -> int {
        QThread::msleep(50);
        return 42;
    });
}

QtCoroutine::QTask<int> withTimeoutFromFuture52() {
    auto task = taskFromQFuture();
    co_return co_await QtCoroutine::withTimeout(task, std::chrono::milliseconds(5000));
}

void test_withTimeout_qfuture_success(QCoreApplication & app) {
    std::cout << "test_withTimeout_qfuture_success\n";

    auto task = withTimeoutFromFuture52();

    QTimer::singleShot(500, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after QFuture completes");
        TEST_ASSERT(task.await_resume() == 42, "should return value from future");
        TEST_ASSERT(!task.isCancelled(), "should NOT be cancelled");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 53. withTimeout + QFuture-backed QTask, timeout fires first
// ====================================================================

QtCoroutine::QTask<int> taskFromSlowQFuture() {
    co_return co_await QtConcurrent::run([]() -> int {
        QThread::msleep(5000);
        return 42;
    });
}

QtCoroutine::QTask<void> withTimeoutFromFutureExpired53() {
    auto task = taskFromSlowQFuture();
    co_await QtCoroutine::withTimeout(task, std::chrono::milliseconds(50));
}

void test_withTimeout_qfuture_expired(QCoreApplication & app) {
    std::cout << "test_withTimeout_qfuture_expired\n";

    auto task = withTimeoutFromFutureExpired53();

    QTimer::singleShot(300, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after timeout");
        TEST_ASSERT(task.isCancelled(), "should report cancelled");
        TEST_ASSERT(task.cancelReason() == QtCoroutine::utils::AwaitCancelled::Timeout, "reason should be Timeout");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 54. withTimeout + QFuture inside a QFuture<int> coroutine
//     (exact noam-core reproduction — QFuture's suspend_never
//     final_suspend destroys the frame on completion)
// ====================================================================

QFuture<int> futureWithTimeout54() {
    auto task = taskFromQFuture();
    auto wrapped = QtCoroutine::withTimeout(task, std::chrono::milliseconds(5000));
    co_return co_await wrapped;
}

void test_withTimeout_qfuture_in_future(QCoreApplication & app) {
    std::cout << "test_withTimeout_qfuture_in_future\n";

    auto future = futureWithTimeout54();

    QTimer::singleShot(500, [&]() {
        TEST_ASSERT(future.isFinished(), "future should be finished");
        TEST_ASSERT(future.result() == 42, "result should be 42");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 55. cancelledBy + QFuture-backed QTask, completes before stop
// ====================================================================

QtCoroutine::QTask<int> cancelledByFromFuture55(std::stop_token st) {
    auto task = taskFromQFuture();
    co_return co_await QtCoroutine::cancelledBy(task, st);
}

void test_cancelledBy_qfuture_success(QCoreApplication & app) {
    std::cout << "test_cancelledBy_qfuture_success\n";

    std::stop_source ss;
    auto task = cancelledByFromFuture55(ss.get_token());

    QTimer::singleShot(500, [&]() {
        TEST_ASSERT(task.await_ready(), "should be done after QFuture completes");
        TEST_ASSERT(task.await_resume() == 42, "should return value from future");
        TEST_ASSERT(!task.isCancelled(), "should NOT be cancelled");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 56. withTimeout — already-completed QTask (sync edge case)
// ====================================================================

void test_withTimeout_already_complete() {
    std::cout << "test_withTimeout_already_complete\n";

    auto inner = taskReturnsValue(); // already done, 42
    auto task = QtCoroutine::withTimeout(inner, std::chrono::milliseconds(5000));
    TEST_ASSERT(task.await_ready(), "already-complete task should be immediately ready");
    TEST_ASSERT(task.await_resume() == 42, "should return 42");
}

// ====================================================================
// 57. .detach() on in-flight task — callback fires after wrapper destructs
// ====================================================================

QtCoroutine::QTask<int> detachInflight57() {
    co_return co_await QtConcurrent::run([]() -> int {
        QThread::msleep(50);
        return 42;
    });
}

void test_detach_inflight(QCoreApplication & app) {
    std::cout << "test_detach_inflight\n";

    bool callbackFired = false;
    int capturedValue = 0;
    {
        auto task = detachInflight57();
        task.then([&](const int & val) {
            callbackFired = true;
            capturedValue = val;
        });
        task.detach();
        // wrapper destructs here; coroutine runs to completion, callback fires
    }

    QTimer::singleShot(200, [&]() {
        TEST_ASSERT(callbackFired, "detached in-flight task callback should fire");
        TEST_ASSERT(capturedValue == 42, "detached task captured value should be 42");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 58. .detach() on already-completed sync task
// ====================================================================

void test_detach_already_done() {
    std::cout << "test_detach_already_done\n";

    bool callbackFired = false;
    int capturedValue = 0;
    {
        auto task = taskReturnsValue(); // returns 42 synchronously, already done
        task.then([&](const int & val) {
            callbackFired = true;
            capturedValue = val;
        });
        TEST_ASSERT(callbackFired, ".then() on done task fires immediately");
        TEST_ASSERT(capturedValue == 42, "captured value should be 42");
        task.detach(); // destroys already-done frame; wrapper destructor becomes a no-op
    }
}

// ====================================================================
// 59. .detach() without any callbacks — frame self-destructs
// ====================================================================

QtCoroutine::QTask<void> detachNoCallback59([[maybe_unused]] std::shared_ptr<int> sentinel) {
    co_await QtConcurrent::run([]() { QThread::msleep(50); });
}

void test_detach_no_callback(QCoreApplication & app) {
    std::cout << "test_detach_no_callback\n";

    auto sentinel = std::make_shared<int>(42);
    std::weak_ptr<int> weak = sentinel;
    {
        auto task = detachNoCallback59(sentinel);
        sentinel.reset(); // caller's ref gone; the frame holds the only ref
        task.detach();
        // wrapper destructs; coroutine completes and the frame self-destructs,
        // releasing the shared_ptr parameter stored in the frame
    }

    QTimer::singleShot(200, [&]() {
        TEST_ASSERT(weak.expired(), "sentinel freed => frame destroyed => coroutine self-destructed");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 60. .onCancelled() + .detach() on cancelled task
// ====================================================================

QtCoroutine::QTask<int> detachCancellable60(Emitter * e, std::stop_token st) {
    co_return co_await QtCoroutine::signal(e, &Emitter::oneArg).cancelledBy(st);
}

void test_detach_onCancelled(QCoreApplication & app) {
    std::cout << "test_detach_onCancelled\n";

    Emitter e;
    std::stop_source ss;
    bool callbackFired = false;
    QtCoroutine::utils::AwaitCancelled::Reason receivedReason{};
    {
        auto task = detachCancellable60(&e, ss.get_token());
        task.onCancelled([&](const QtCoroutine::utils::AwaitCancelled & c) {
            callbackFired = true;
            receivedReason = c.reason;
        });
        task.detach();
    }

    QTimer::singleShot(10, [&]() { ss.request_stop(); });
    QTimer::singleShot(100, [&]() {
        TEST_ASSERT(callbackFired, "onCancelled should fire on detached cancelled task");
        TEST_ASSERT(receivedReason == QtCoroutine::utils::AwaitCancelled::Stopped, "reason should be Stopped");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 61. .onError() + .detach() on throwing task
// ====================================================================

QtCoroutine::QTask<void> detachThrowing61() {
    co_await QtCoroutine::sleep(std::chrono::milliseconds(10));
    throw std::runtime_error("detached error");
}

void test_detach_onError(QCoreApplication & app) {
    std::cout << "test_detach_onError\n";

    bool callbackFired = false;
    std::string errorMsg;
    {
        auto task = detachThrowing61();
        task.onError([&](std::exception_ptr ep) {
            callbackFired = true;
            try {
                std::rethrow_exception(ep);
            } catch (const std::runtime_error & ex) {
                errorMsg = ex.what();
            }
        });
        task.detach();
    }

    QTimer::singleShot(100, [&]() {
        TEST_ASSERT(callbackFired, "onError should fire on detached throwing task");
        TEST_ASSERT(errorMsg == "detached error", "error message should match");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 62. Double-detach is a no-op
// ====================================================================

QtCoroutine::QTask<int> detachTwice62() {
    co_return co_await QtConcurrent::run([]() -> int {
        QThread::msleep(50);
        return 42;
    });
}

void test_detach_double(QCoreApplication & app) {
    std::cout << "test_detach_double\n";

    int callCount = 0;
    {
        auto task = detachTwice62();
        task.then([&](const int &) { ++callCount; });
        task.detach();
        task.detach(); // second detach should be a no-op — no crash
    }

    QTimer::singleShot(200, [&]() {
        TEST_ASSERT(callCount == 1, "callback should fire exactly once after double detach");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 63. Non-detached destruction still cancels (regression guard)
// ====================================================================

QtCoroutine::QTask<int> detachRegression63([[maybe_unused]] std::shared_ptr<int> sentinel) {
    co_await QtConcurrent::run([]() { QThread::msleep(50); });
    co_return 42;
}

void test_detach_regression_nondetached(QCoreApplication & app) {
    std::cout << "test_detach_regression_nondetached\n";

    auto sentinel = std::make_shared<int>(42);
    std::weak_ptr<int> weak = sentinel;
    bool callbackFired = false;
    {
        auto task = detachRegression63(sentinel);
        sentinel.reset(); // caller's ref gone
        task.then([&](const int &) { callbackFired = true; });
        // DO NOT detach — wrapper destructs, destroying the frame synchronously
    }

    QTimer::singleShot(200, [&]() {
        TEST_ASSERT(weak.expired(), "frame destroyed by wrapper => sentinel freed");
        TEST_ASSERT(!callbackFired, "callback must NOT fire (frame died before return_value)");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 64. co_await on a QTask still works (regression guard for non-detach path)
// ====================================================================

QtCoroutine::QTask<int> innerTask64() {
    co_return co_await QtConcurrent::run([]() -> int {
        QThread::msleep(50);
        return 42;
    });
}

QtCoroutine::QTask<int> outerTask64() {
    co_return co_await innerTask64();
}

void test_detach_regression_coawait(QCoreApplication & app) {
    std::cout << "test_detach_regression_coawait\n";

    auto task = outerTask64();

    QTimer::singleShot(200, [&]() {
        TEST_ASSERT(task.await_ready(), "outer task should be done");
        TEST_ASSERT(task.await_resume() == 42, "outer task should return 42");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// main
// ====================================================================

// ====================================================================
// 65. Pre-requested stop_token cancels at co_await (regression: used to
//     take the success path — silent success for 0-arg signals, empty-
//     optional dereference (UB) for N-arg signals)
// ====================================================================

QtCoroutine::QTask<void> awaitPreStoppedVoid(Emitter * e, std::stop_token st) {
    co_await QtCoroutine::signal(e, &Emitter::voidSignal).cancelledBy(st);
}

QtCoroutine::QTask<int> awaitPreStoppedOneArg(Emitter * e, std::stop_token st) {
    co_return co_await QtCoroutine::signal(e, &Emitter::oneArg).cancelledBy(st);
}

void test_precancelled_stop_token() {
    std::cout << "test_precancelled_stop_token\n";

    Emitter e;
    std::stop_source ss;
    ss.request_stop();

    auto t1 = awaitPreStoppedVoid(&e, ss.get_token());
    TEST_ASSERT(t1.await_ready(), "pre-stopped 0-arg await settles immediately");
    TEST_ASSERT(t1.isCancelled(), "pre-stopped 0-arg await is cancelled");
    TEST_ASSERT(t1.cancelReason() == QtCoroutine::utils::AwaitCancelled::Stopped, "0-arg reason is Stopped");

    auto t2 = awaitPreStoppedOneArg(&e, ss.get_token());
    TEST_ASSERT(t2.await_ready(), "pre-stopped 1-arg await settles immediately");
    TEST_ASSERT(t2.isCancelled(), "pre-stopped 1-arg await is cancelled");
    TEST_ASSERT(t2.cancelReason() == QtCoroutine::utils::AwaitCancelled::Stopped, "1-arg reason is Stopped");
}

// ====================================================================
// 66. co_await QFuture: exception delivered while suspended (regression:
//     Qt skips value-signature then-continuations on exception, so the
//     coroutine never resumed; also exception futures report isCanceled()
//     and were misreported as AwaitCancelled on the ready path)
// ====================================================================

QtCoroutine::QTask<int> awaitIntFuture(QFuture<int> f) {
    co_return co_await f;
}

void test_qfuture_exception_while_suspended(QCoreApplication & app) {
    std::cout << "test_qfuture_exception_while_suspended\n";

    QPromise<int> promise;
    promise.start();
    auto task = awaitIntFuture(promise.future());
    TEST_ASSERT(!task.await_ready(), "should be suspended on unfinished future");

    QTimer::singleShot(10, [&]() {
        promise.setException(std::make_exception_ptr(std::runtime_error("future failed")));
        promise.finish();
    });
    QTimer::singleShot(100, [&]() {
        TEST_ASSERT(task.await_ready(), "should resume after future exception");
        if (task.await_ready()) {
            TEST_ASSERT(!task.isCancelled(), "exception is an error, not a cancellation");
            bool caught = false;
            try {
                task.await_resume();
            } catch (std::runtime_error & e) {
                caught = std::string(e.what()) == "future failed";
            } catch (...) {}
            TEST_ASSERT(caught, "original exception propagates through co_await");
        }
        app.quit();
    });

    app.exec();
}

QtCoroutine::QTask<int> awaitConcurrentThrow() {
    auto f = QtConcurrent::run([]() -> int {
        QThread::msleep(30);
        throw std::runtime_error("worker threw");
    });
    co_return co_await f;
}

void test_qfuture_exception_from_worker(QCoreApplication & app) {
    std::cout << "test_qfuture_exception_from_worker\n";

    auto task = awaitConcurrentThrow();

    QTimer::singleShot(300, [&]() {
        TEST_ASSERT(task.await_ready(), "should resume after worker exception");
        if (task.await_ready()) {
            TEST_ASSERT(!task.isCancelled(), "worker exception is an error, not a cancellation");
            bool caught = false;
            try {
                task.await_resume();
            } catch (std::runtime_error & e) {
                caught = std::string(e.what()) == "worker threw";
            } catch (...) {}
            TEST_ASSERT(caught, "QUnhandledException is unwrapped to the original exception");
        }
        app.quit();
    });

    app.exec();
}

void test_qfuture_already_failed() {
    std::cout << "test_qfuture_already_failed\n";

    QPromise<int> promise;
    promise.start();
    promise.setException(std::make_exception_ptr(std::runtime_error("already failed")));
    promise.finish();

    auto task = awaitIntFuture(promise.future());
    TEST_ASSERT(task.await_ready(), "already-failed future settles immediately");
    TEST_ASSERT(!task.isCancelled(), "stored exception beats the isCanceled() check");
    bool caught = false;
    try {
        task.await_resume();
    } catch (std::runtime_error & e) {
        caught = std::string(e.what()) == "already failed";
    } catch (...) {}
    TEST_ASSERT(caught, "exception propagates on the ready path");
}

// ====================================================================
// 67. co_await QFuture: canceled while suspended (regression: then-
//     continuations never run on cancellation, so the coroutine hung)
// ====================================================================

void test_qfuture_cancel_while_suspended(QCoreApplication & app) {
    std::cout << "test_qfuture_cancel_while_suspended\n";

    QPromise<int> promise;
    promise.start();
    auto task = awaitIntFuture(promise.future());
    TEST_ASSERT(!task.await_ready(), "should be suspended on unfinished future");

    // cancel() alone does not run continuations on Qt 6.4 — the resume
    // happens once the canceled future settles (finish, or QPromise
    // destruction, or the canceled worker completing).
    QTimer::singleShot(10, [&]() {
        promise.future().cancel();
        promise.finish();
    });
    QTimer::singleShot(100, [&]() {
        TEST_ASSERT(task.await_ready(), "should resume after future cancel");
        if (task.await_ready()) {
            TEST_ASSERT(task.isCancelled(), "cancel maps to cancelled state");
            TEST_ASSERT(task.cancelReason() == QtCoroutine::utils::AwaitCancelled::Stopped, "reason should be Stopped");
        }
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 68. Task destroyed between request_stop() and the event-loop spin
//     (regression: the stop callback posts a queued resume; the guard
//     was a local of await_suspend, so destroying the frame could not
//     mark it stale and the posted call resumed freed memory — UAF
//     under ASan)
// ====================================================================

void test_stop_then_destroy_before_resume(QCoreApplication & app) {
    std::cout << "test_stop_then_destroy_before_resume\n";

    Emitter e;
    std::stop_source ss;
    {
        auto task = awaitWithCancel(&e, ss.get_token());
        TEST_ASSERT(!task.await_ready(), "should be suspended");
        ss.request_stop(); // posts the queued resume
    } // task (and coroutine frame) destroyed before the loop spins

    // Deliver the stale queued resume — it must be dropped, not fire
    // into the destroyed frame.
    QTimer::singleShot(50, [&]() { app.quit(); });
    app.exec();
    TEST_ASSERT(true, "no use-after-free resuming after stop+destroy");
}

// ====================================================================
// 69. Task destroyed while suspended in sleep() (regression: the timer
//     callback captured the raw handle with no guard and resumed a
//     destroyed frame — UAF under ASan)
// ====================================================================

QtCoroutine::QTask<void> sleepingTask(bool * reached) {
    co_await QtCoroutine::sleep(std::chrono::milliseconds(40));
    *reached = true;
}

void test_sleep_destroyed_while_suspended(QCoreApplication & app) {
    std::cout << "test_sleep_destroyed_while_suspended\n";

    bool reached = false;
    {
        auto task = sleepingTask(&reached);
        TEST_ASSERT(!task.await_ready(), "should be sleeping");
    } // frame destroyed mid-sleep; the single-shot timer is still pending

    // Let the stale timer fire — it must be dropped.
    QTimer::singleShot(100, [&]() { app.quit(); });
    app.exec();
    TEST_ASSERT(!reached, "destroyed sleeper must not run past the sleep");
}

// ====================================================================
// 70. Builder order: withTimeout() before readyIf() (regression: the
//     readyIf() rebuild dropped m_timeout, so the timeout never armed
//     and the await could suspend forever)
// ====================================================================

QtCoroutine::QTask<int> awaitTimeoutThenReadyIf(Emitter * e) {
    co_return co_await QtCoroutine::signal(e, &Emitter::oneArg)
        .withTimeout(std::chrono::milliseconds(20))
        .readyIf([](Emitter *) { return std::optional<int>{}; });
}

QtCoroutine::QTask<int> awaitReadyIfThenTimeout(Emitter * e) {
    co_return co_await QtCoroutine::signal(e, &Emitter::oneArg)
        .readyIf([](Emitter *) { return std::optional<int>{}; })
        .withTimeout(std::chrono::milliseconds(20));
}

void test_withTimeout_readyIf_order(QCoreApplication & app) {
    std::cout << "test_withTimeout_readyIf_order\n";

    Emitter e;
    auto t1 = awaitTimeoutThenReadyIf(&e);
    auto t2 = awaitReadyIfThenTimeout(&e);
    TEST_ASSERT(!t1.await_ready(), "t1 suspends: check not ready, no signal yet");
    TEST_ASSERT(!t2.await_ready(), "t2 suspends: check not ready, no signal yet");

    QTimer::singleShot(100, [&]() {
        TEST_ASSERT(t1.await_ready(), "withTimeout().readyIf() must keep the timeout");
        if (t1.await_ready()) {
            TEST_ASSERT(t1.isCancelled(), "t1 should be cancelled");
            TEST_ASSERT(t1.cancelReason() == QtCoroutine::utils::AwaitCancelled::Timeout, "t1 reason is Timeout");
        }
        TEST_ASSERT(t2.await_ready(), "readyIf().withTimeout() times out");
        if (t2.await_ready()) {
            TEST_ASSERT(t2.isCancelled(), "t2 should be cancelled");
            TEST_ASSERT(t2.cancelReason() == QtCoroutine::utils::AwaitCancelled::Timeout, "t2 reason is Timeout");
        }
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 71. whenAll/whenAny awaiter destroyed while suspended (regression:
//     the callbacks registered on the caller-owned tasks had no
//     destructor guard — whenAll's onComplete had no guard at all —
//     and a later task completion resumed the destroyed frame; UAF
//     under ASan)
// ====================================================================

QtCoroutine::QTask<int> waitOneArg71(Emitter * e) {
    co_return co_await QtCoroutine::signal(e, &Emitter::oneArg);
}

QtCoroutine::QTask<void> waitVoid71(Emitter * e) {
    co_await QtCoroutine::signal(e, &Emitter::voidSignal);
}

QtCoroutine::QTask<void> whenAllWaiter71(QtCoroutine::QTask<int> & a, QtCoroutine::QTask<int> & b, bool * reached) {
    co_await QtCoroutine::whenAll(a, b);
    *reached = true;
}

QtCoroutine::QTask<void> whenAllVoidWaiter71(QtCoroutine::QTask<void> & a, QtCoroutine::QTask<void> & b,
                                             bool * reached) {
    co_await QtCoroutine::whenAll(a, b);
    *reached = true;
}

QtCoroutine::QTask<void> whenAnyWaiter71(QtCoroutine::QTask<int> & a, QtCoroutine::QTask<int> & b, bool * reached) {
    co_await QtCoroutine::whenAny(a, b);
    *reached = true;
}

void test_whenAll_awaiter_destroyed() {
    std::cout << "test_whenAll_awaiter_destroyed\n";

    Emitter e1, e2;
    auto t1 = waitOneArg71(&e1);
    auto t2 = waitOneArg71(&e2);

    bool reached = false;
    {
        auto waiter = whenAllWaiter71(t1, t2, &reached);
        TEST_ASSERT(!waiter.await_ready(), "whenAll waiter should be suspended");
    } // waiter frame destroyed; t1/t2 still hold its callbacks

    emit e1.oneArg(1);
    emit e2.oneArg(2); // last completion fires the stale onComplete — must be dropped

    TEST_ASSERT(t1.await_ready() && t2.await_ready(), "inner tasks completed normally");
    TEST_ASSERT(!reached, "destroyed whenAll waiter must not resume");
}

void test_whenAll_void_awaiter_destroyed() {
    std::cout << "test_whenAll_void_awaiter_destroyed\n";

    Emitter e1, e2;
    auto t1 = waitVoid71(&e1);
    auto t2 = waitVoid71(&e2);

    bool reached = false;
    {
        auto waiter = whenAllVoidWaiter71(t1, t2, &reached);
        TEST_ASSERT(!waiter.await_ready(), "void whenAll waiter should be suspended");
    }

    emit e1.voidSignal();
    emit e2.voidSignal();

    TEST_ASSERT(t1.await_ready() && t2.await_ready(), "inner void tasks completed normally");
    TEST_ASSERT(!reached, "destroyed void whenAll waiter must not resume");
}

void test_whenAny_awaiter_destroyed() {
    std::cout << "test_whenAny_awaiter_destroyed\n";

    Emitter e1, e2;
    auto t1 = waitOneArg71(&e1);
    auto t2 = waitOneArg71(&e2);

    bool reached = false;
    {
        auto waiter = whenAnyWaiter71(t1, t2, &reached);
        TEST_ASSERT(!waiter.await_ready(), "whenAny waiter should be suspended");
    }

    emit e1.oneArg(1); // first completion — stale resume must be dropped

    TEST_ASSERT(!reached, "destroyed whenAny waiter must not resume");
}

// ====================================================================
// 72. whenAny overload constraints: all-void and mixed value types are
//     supported; void/value mixes are rejected at the constraint (the
//     original regression: the old constraint admitted QTask<void> and
//     instantiation failed deep inside WhenAnyAwaitable with "forming
//     reference to void")
// ====================================================================

// Must be a template: only a dependent requires-expression turns the
// failed overload resolution into `false` instead of a hard error.
template<typename T>
constexpr bool whenAnyCompilesFor =
    requires(QtCoroutine::QTask<T> & a, QtCoroutine::QTask<T> & b) { QtCoroutine::whenAny(a, b); };

static_assert(whenAnyCompilesFor<void>, "whenAny accepts all-void task packs");
static_assert(whenAnyCompilesFor<int>, "whenAny accepts homogeneous non-void tasks");

// Mixing void and value tasks stays cleanly rejected at the constraint.
template<typename A, typename B>
constexpr bool whenAnyCompilesForMixed =
    requires(QtCoroutine::QTask<A> & a, QtCoroutine::QTask<B> & b) { QtCoroutine::whenAny(a, b); };

static_assert(whenAnyCompilesForMixed<int, QString>, "whenAny accepts mixed value types (variant)");
static_assert(!whenAnyCompilesForMixed<int, void>, "whenAny must cleanly reject void/value mixes");
static_assert(!whenAnyCompilesForMixed<void, int>, "whenAny must cleanly reject void/value mixes");

// ====================================================================
// 73. Cross-thread sender: the coroutine resumes on the awaiting thread
//     (regression: the default connection context used to be the sender,
//     so the coroutine silently migrated to the sender's thread)
// ====================================================================

QtCoroutine::QTask<int> awaitOneArgRecordThread73(Emitter * e, QThread ** resumedOn) {
    int v = co_await QtCoroutine::signal(e, &Emitter::oneArg);
    *resumedOn = QThread::currentThread();
    co_return v;
}

void test_cross_thread_sender_resumes_on_awaiting_thread(QCoreApplication & app) {
    std::cout << "test_cross_thread_sender_resumes_on_awaiting_thread\n";

    QThread worker;
    Emitter e;
    e.moveToThread(&worker); // legal while the thread is not yet running

    QThread * resumedOn = nullptr;
    auto task = awaitOneArgRecordThread73(&e, &resumedOn);
    TEST_ASSERT(!task.await_ready(), "should be suspended");

    // Start the worker only after the await is armed: thread creation gives
    // TSan a visible happens-before edge from the setup to the emission.
    worker.start();
    QMetaObject::invokeMethod(&e, [&e]() { emit e.oneArg(42); }, Qt::QueuedConnection);

    QTimer::singleShot(200, [&]() { app.quit(); });
    app.exec();

    TEST_ASSERT(task.await_ready(), "should resume after cross-thread emission");
    if (task.await_ready()) {
        TEST_ASSERT(task.await_resume() == 42, "signal value crosses threads");
        TEST_ASSERT(resumedOn == QThread::currentThread(), "code after co_await stays on the awaiting thread");
    }

    worker.quit();
    worker.wait();
}

// ====================================================================
// 74. Cross-thread sender destroyed: cancellation delivered on the
//     awaiting thread
// ====================================================================

QtCoroutine::QTask<void> awaitVoidRecordThread74(Emitter * e, QThread ** resumedOn) {
    try {
        co_await QtCoroutine::signal(e, &Emitter::voidSignal);
    } catch (...) {
        *resumedOn = QThread::currentThread();
        throw;
    }
}

void test_cross_thread_sender_destroyed(QCoreApplication & app) {
    std::cout << "test_cross_thread_sender_destroyed\n";

    QThread worker;
    auto * e = new Emitter;
    e->moveToThread(&worker);

    QThread * resumedOn = nullptr;
    auto task = awaitVoidRecordThread74(e, &resumedOn);
    TEST_ASSERT(!task.await_ready(), "should be suspended");

    worker.start();                                                          // after setup — see test 73
    QMetaObject::invokeMethod(e, [e]() { delete e; }, Qt::QueuedConnection); // delete on worker thread

    QTimer::singleShot(200, [&]() { app.quit(); });
    app.exec();

    TEST_ASSERT(task.await_ready(), "should resume after cross-thread sender destruction");
    if (task.await_ready()) {
        TEST_ASSERT(task.isCancelled(), "should report cancelled");
        TEST_ASSERT(task.cancelReason() == QtCoroutine::utils::AwaitCancelled::SenderDestroyed,
                    "reason should be SenderDestroyed");
        TEST_ASSERT(resumedOn == QThread::currentThread(), "cancellation unwinds on the awaiting thread");
    }

    worker.quit();
    worker.wait();
}

// ====================================================================
// 75. Cross-thread sender + withTimeout: timeout fires on the awaiting
//     thread (regression: the QTimer used to be torn down from the
//     resume thread, which was unsupported cross-thread)
// ====================================================================

QtCoroutine::QTask<void> awaitWithTimeoutRecordThread75(Emitter * e, QThread ** resumedOn) {
    try {
        co_await QtCoroutine::signal(e, &Emitter::voidSignal).withTimeout(std::chrono::milliseconds(30));
    } catch (...) {
        *resumedOn = QThread::currentThread();
        throw;
    }
}

void test_cross_thread_sender_timeout(QCoreApplication & app) {
    std::cout << "test_cross_thread_sender_timeout\n";

    QThread worker;
    Emitter e;
    e.moveToThread(&worker); // never emits

    QThread * resumedOn = nullptr;
    auto task = awaitWithTimeoutRecordThread75(&e, &resumedOn);
    TEST_ASSERT(!task.await_ready(), "should be suspended");

    worker.start(); // after setup — see test 73

    QTimer::singleShot(200, [&]() { app.quit(); });
    app.exec();

    TEST_ASSERT(task.await_ready(), "should resume after timeout");
    if (task.await_ready()) {
        TEST_ASSERT(task.isCancelled(), "should report cancelled");
        TEST_ASSERT(task.cancelReason() == QtCoroutine::utils::AwaitCancelled::Timeout, "reason should be Timeout");
        TEST_ASSERT(resumedOn == QThread::currentThread(), "timeout unwinds on the awaiting thread");
    }

    worker.quit();
    worker.wait();
}

// ====================================================================
// 76. resumeOn(ctx) migrates the continuation to ctx's thread — the
//     explicit opt-in counterpart of test 73. Result observed via the
//     toFuture() bridge (the sanctioned cross-thread consumption path).
// ====================================================================

QtCoroutine::QTask<void> migrateToCtx76(Emitter * e, QObject * ctx, QThread ** resumedOn) {
    co_await QtCoroutine::signal(e, &Emitter::voidSignal).resumeOn(ctx);
    *resumedOn = QThread::currentThread();
}

void test_resumeOn_migrates_to_ctx_thread(QCoreApplication & app) {
    std::cout << "test_resumeOn_migrates_to_ctx_thread\n";

    QThread worker;
    worker.start();
    while (!worker.eventDispatcher()) // resumeOn requires a live dispatcher on ctx's thread
        QThread::msleep(1);

    Emitter e; // sender stays on the main thread
    QObject ctx;
    ctx.moveToThread(&worker);

    QThread * resumedOn = nullptr;
    auto task = migrateToCtx76(&e, &ctx, &resumedOn);
    TEST_ASSERT(!task.await_ready(), "should be suspended");

    // Bridge completion back to the main thread before emitting.
    auto future = task.toFuture();
    future.then(&app, [&]() { app.quit(); });

    emit e.voidSignal(); // main-thread emit, queued to ctx's (worker) thread

    QTimer::singleShot(500, [&]() { app.quit(); }); // watchdog
    app.exec();

    TEST_ASSERT(resumedOn == &worker, "code after co_await runs on the resumeOn ctx's thread");

    worker.quit();
    worker.wait();
}

// ====================================================================
// 77. Reassigning a task from inside its own .then() callback — the
//     "deleteLater from your own slot" case (regression: callbacks used
//     to fire from return_value with the coroutine still running, so
//     destroying the frame there was UB)
// ====================================================================

void test_reassign_task_inside_callback() {
    std::cout << "test_reassign_task_inside_callback\n";

    Emitter e;
    std::optional<QtCoroutine::QTask<int>> slot;
    slot.emplace(waitOneArg71(&e));
    TEST_ASSERT(!slot->await_ready(), "first task suspended");

    bool callbackRan = false;
    slot->then([&](const int & v) {
        TEST_ASSERT(v == 11, "first task result delivered");
        // Destroys the first task from inside its own completion callback.
        // Frame destruction is deferred to the dispatcher — no UAF, no leak.
        slot.emplace(taskReturnsValue());
        callbackRan = true;
    });

    emit e.oneArg(11);

    TEST_ASSERT(callbackRan, "callback ran");
    TEST_ASSERT(slot->await_ready(), "replacement task settled");
    TEST_ASSERT(slot->await_resume() == 42, "replacement task usable immediately");
}

// ====================================================================
// 78. detach() from inside the task's own callback: the completion
//     dispatch self-destructs the frame (no leak, no double-destroy)
// ====================================================================

void test_detach_inside_callback() {
    std::cout << "test_detach_inside_callback\n";

    Emitter e;
    std::optional<QtCoroutine::QTask<int>> slot;
    slot.emplace(waitOneArg71(&e));

    bool callbackRan = false;
    slot->then([&](const int &) {
        slot->detach(); // mid-dispatch detach: dispatcher self-destructs the frame
        callbackRan = true;
    });

    emit e.oneArg(5);
    TEST_ASSERT(callbackRan, "callback ran and detach from inside it did not crash");
}

// ====================================================================
// 79. Cross-thread completion (resumeOn migration): a then() registered
//     before completion fires on the completion thread; one registered
//     after settling fires immediately on the registering thread. The
//     promise mutex makes both registrations race-free.
// ====================================================================

QtCoroutine::QTask<int> migrateAndReturn79(Emitter * e, QObject * ctx) {
    int v = co_await QtCoroutine::signal(e, &Emitter::oneArg).resumeOn(ctx);
    co_return v; // completes on ctx's thread
}

void test_then_with_cross_thread_completion(QCoreApplication & app) {
    std::cout << "test_then_with_cross_thread_completion\n";

    QThread worker;
    worker.start();
    while (!worker.eventDispatcher())
        QThread::msleep(1);

    Emitter e; // sender on the main thread
    QObject ctx;
    ctx.moveToThread(&worker);

    auto task = migrateAndReturn79(&e, &ctx);

    std::atomic<QThread *> preThread{nullptr};
    task.then([&](const int & v) {
        if (v == 9)
            preThread.store(QThread::currentThread(), std::memory_order_release);
    });

    emit e.oneArg(9); // queued to the worker; the task completes there

    QTimer::singleShot(300, [&]() { app.quit(); });
    app.exec();

    TEST_ASSERT(task.await_ready(), "task settled (cross-thread settled read)");
    TEST_ASSERT(preThread.load(std::memory_order_acquire) == &worker,
                "pre-registered then fires on the completion thread");

    QThread * postThread = nullptr;
    int postVal = 0;
    task.then([&](const int & v) {
        postVal = v;
        postThread = QThread::currentThread();
    });
    TEST_ASSERT(postVal == 9, "post-settle then fires immediately with the value");
    TEST_ASSERT(postThread == QThread::currentThread(), "post-settle then runs on the registering thread");

    worker.quit();
    worker.wait();
}

// ====================================================================
// 80. waitFor — synchronous bridge: blocks on a nested event loop and
//     returns the result or rethrows cancellation/error. Note: no
//     app.exec() scaffolding needed; waitFor pumps the timers itself.
// ====================================================================

QtCoroutine::QTask<void> failingAfterSleep80() {
    co_await QtCoroutine::sleep(std::chrono::milliseconds(5));
    throw std::runtime_error("waitFor error");
}

void test_waitFor() {
    std::cout << "test_waitFor\n";

    // Already-settled task: returns without spinning.
    auto done = taskReturnsValue();
    TEST_ASSERT(QtCoroutine::waitFor(done) == 42, "waitFor returns a settled value immediately");

    // Pending task: waitFor spins until the signal arrives.
    Emitter e;
    auto pending = waitOneArg71(&e);
    QTimer::singleShot(10, [&]() { emit e.oneArg(7); });
    TEST_ASSERT(QtCoroutine::waitFor(pending) == 7, "waitFor blocks until the task settles");

    // Rvalue task.
    QTimer::singleShot(10, [&]() { emit e.oneArg(8); });
    TEST_ASSERT(QtCoroutine::waitFor(waitOneArg71(&e)) == 8, "waitFor accepts a temporary task");

    // Cancellation surfaces as AwaitCancelled.
    std::stop_source ss;
    auto cancelled = awaitWithCancel(&e, ss.get_token());
    QTimer::singleShot(10, [&]() { ss.request_stop(); });
    bool caught = false;
    try {
        QtCoroutine::waitFor(cancelled);
    } catch (QtCoroutine::utils::AwaitCancelled & c) {
        caught = c.reason == QtCoroutine::utils::AwaitCancelled::Stopped;
    }
    TEST_ASSERT(caught, "waitFor rethrows cancellation as AwaitCancelled");

    // Errors propagate as the original exception.
    auto failing = failingAfterSleep80();
    bool errCaught = false;
    try {
        QtCoroutine::waitFor(failing);
    } catch (std::runtime_error & ex) {
        errCaught = std::string(ex.what()) == "waitFor error";
    }
    TEST_ASSERT(errCaught, "waitFor rethrows task errors");
}

// ====================================================================
// 81. whenAny over QTask<void>: returns the winner's index; a winner
//     that settled by cancellation rethrows
// ====================================================================

QtCoroutine::QTask<std::size_t> whenAnyVoid81(QtCoroutine::QTask<void> & a, QtCoroutine::QTask<void> & b) {
    co_return co_await QtCoroutine::whenAny(a, b);
}

void test_whenAny_void_index() {
    std::cout << "test_whenAny_void_index\n";

    Emitter e1, e2;
    auto t1 = waitVoid71(&e1);
    auto t2 = waitVoid71(&e2);
    auto winner = whenAnyVoid81(t1, t2);
    TEST_ASSERT(!winner.await_ready(), "void whenAny should be suspended");

    emit e2.voidSignal();

    TEST_ASSERT(winner.await_ready(), "void whenAny settles on first completion");
    TEST_ASSERT(winner.await_resume() == 1, "void whenAny returns the winner's index");
}

void test_whenAny_void_cancelled() {
    std::cout << "test_whenAny_void_cancelled\n";

    Emitter e;
    std::stop_source ss;
    auto t1 = awaitWithCancel(&e, ss.get_token());
    auto t2 = waitVoid71(&e);
    auto winner = whenAnyVoid81(t1, t2);
    TEST_ASSERT(!winner.await_ready(), "should be suspended");

    ss.request_stop(); // queued resume — waitFor pumps it

    bool caught = false;
    try {
        QtCoroutine::waitFor(winner);
    } catch (QtCoroutine::utils::AwaitCancelled & c) {
        caught = c.reason == QtCoroutine::utils::AwaitCancelled::Stopped;
    }
    TEST_ASSERT(caught, "whenAny rethrows the winner's cancellation");
}

// ====================================================================
// 82. whenAny over mixed value types: std::variant with the winner at
//     variant::index(); a winner that settled by error rethrows
// ====================================================================

QtCoroutine::QTask<QString> waitTwoArgsMsg82(Emitter * e) {
    auto [ok, msg] = co_await QtCoroutine::signal(e, &Emitter::twoArgs);
    co_return msg;
}

QtCoroutine::QTask<std::variant<int, QString>> whenAnyVariant82(QtCoroutine::QTask<int> & a,
                                                                QtCoroutine::QTask<QString> & b) {
    co_return co_await QtCoroutine::whenAny(a, b);
}

void test_whenAny_variant() {
    std::cout << "test_whenAny_variant\n";

    // String task wins.
    {
        Emitter e1, e2;
        auto t1 = waitOneArg71(&e1);
        auto t2 = waitTwoArgsMsg82(&e2);
        auto race = whenAnyVariant82(t1, t2);
        TEST_ASSERT(!race.await_ready(), "variant whenAny should be suspended");

        emit e2.twoArgs(true, "fast");

        TEST_ASSERT(race.await_ready(), "variant whenAny settles");
        auto v = race.await_resume();
        TEST_ASSERT(v.index() == 1, "winner position via variant::index()");
        TEST_ASSERT(std::get<1>(v) == "fast", "winner value held in the variant");
    }

    // Int task wins.
    {
        Emitter e1, e2;
        auto t1 = waitOneArg71(&e1);
        auto t2 = waitTwoArgsMsg82(&e2);
        auto race = whenAnyVariant82(t1, t2);

        emit e1.oneArg(99);

        TEST_ASSERT(race.await_ready(), "variant whenAny settles");
        auto v = race.await_resume();
        TEST_ASSERT(v.index() == 0, "winner position via variant::index()");
        TEST_ASSERT(std::get<0>(v) == 99, "winner value held in the variant");
    }
}

QtCoroutine::QTask<int> failingInt82(Emitter * e) {
    co_await QtCoroutine::signal(e, &Emitter::voidSignal);
    throw std::runtime_error("variant winner failed");
}

void test_whenAny_variant_error() {
    std::cout << "test_whenAny_variant_error\n";

    Emitter e1, e2;
    auto t1 = failingInt82(&e1);
    auto t2 = waitTwoArgsMsg82(&e2);
    auto race = whenAnyVariant82(t1, t2);
    TEST_ASSERT(!race.await_ready(), "should be suspended");

    emit e1.voidSignal(); // t1 settles first — with an error

    TEST_ASSERT(race.await_ready(), "settles on the failing winner");
    bool caught = false;
    try {
        race.await_resume();
    } catch (std::runtime_error & ex) {
        caught = std::string(ex.what()) == "variant winner failed";
    }
    TEST_ASSERT(caught, "whenAny rethrows the winner's error");
}

// ====================================================================
// 83. Destroyed-awaiter guards for the void and variant whenAny
//     awaitables (mirrors test 71 for the homogeneous one)
// ====================================================================

QtCoroutine::QTask<void> whenAnyVoidWaiter83(QtCoroutine::QTask<void> & a, QtCoroutine::QTask<void> & b,
                                             bool * reached) {
    co_await QtCoroutine::whenAny(a, b);
    *reached = true;
}

QtCoroutine::QTask<void> whenAnyVariantWaiter83(QtCoroutine::QTask<int> & a, QtCoroutine::QTask<QString> & b,
                                                bool * reached) {
    co_await QtCoroutine::whenAny(a, b);
    *reached = true;
}

void test_whenAny_void_awaiter_destroyed() {
    std::cout << "test_whenAny_void_awaiter_destroyed\n";

    Emitter e1, e2;
    auto t1 = waitVoid71(&e1);
    auto t2 = waitVoid71(&e2);

    bool reached = false;
    {
        auto waiter = whenAnyVoidWaiter83(t1, t2, &reached);
        TEST_ASSERT(!waiter.await_ready(), "void whenAny waiter should be suspended");
    }

    emit e1.voidSignal(); // stale resume must be dropped

    TEST_ASSERT(!reached, "destroyed void whenAny waiter must not resume");
}

void test_whenAny_variant_awaiter_destroyed() {
    std::cout << "test_whenAny_variant_awaiter_destroyed\n";

    Emitter e1, e2;
    auto t1 = waitOneArg71(&e1);
    auto t2 = waitTwoArgsMsg82(&e2);

    bool reached = false;
    {
        auto waiter = whenAnyVariantWaiter83(t1, t2, &reached);
        TEST_ASSERT(!waiter.await_ready(), "variant whenAny waiter should be suspended");
    }

    emit e1.oneArg(1); // stale resume must be dropped

    TEST_ASSERT(!reached, "destroyed variant whenAny waiter must not resume");
}

// ====================================================================
// 84. API hygiene: namespace alias, optional cancelReason, contained
//     callback exceptions
// ====================================================================

void test_hygiene_api() {
    std::cout << "test_hygiene_api\n";

    static_assert(std::is_same_v<QtCoroutine::AwaitCancelled, QtCoroutine::utils::AwaitCancelled>,
                  "AwaitCancelled is aliased at namespace scope");

    // cancelReason() engages only on cancelled tasks.
    auto ok = taskReturnsValue();
    TEST_ASSERT(!ok.isCancelled(), "successful task not cancelled");
    TEST_ASSERT(!ok.cancelReason().has_value(), "cancelReason empty on a successful task");

    Emitter e;
    std::stop_source ss;
    ss.request_stop();
    auto cancelled = awaitPreStoppedVoid(&e, ss.get_token());
    TEST_ASSERT(cancelled.isCancelled(), "pre-stopped task cancelled");
    TEST_ASSERT(cancelled.cancelReason().has_value(), "cancelReason engaged on a cancelled task");
    TEST_ASSERT(cancelled.cancelReason() == QtCoroutine::AwaitCancelled::Stopped,
                "optional<Reason> compares against plain Reason");

    // A throwing callback is contained (qWarning'd) and the task settles.
    Emitter e2;
    auto pending = waitOneArg71(&e2);
    pending.then([](const int &) { throw std::runtime_error("callback boom"); });
    emit e2.oneArg(1);
    TEST_ASSERT(pending.await_ready(), "task settles despite a throwing callback");
    TEST_ASSERT(!pending.isCancelled(), "task state intact despite a throwing callback");
}

// ====================================================================
// 85. Cross-thread request_stop on a signal await: the stop callback
//     runs on the requesting thread; the cancellation must marshal
//     back to the awaiting thread (README's "inputs are unrestricted")
// ====================================================================

QtCoroutine::QTask<void> awaitVoidCancellableRecordThread85(Emitter * e, std::stop_token st, QThread ** resumedOn) {
    try {
        co_await QtCoroutine::signal(e, &Emitter::voidSignal).cancelledBy(st);
    } catch (...) {
        *resumedOn = QThread::currentThread();
        throw;
    }
}

void test_cross_thread_request_stop(QCoreApplication & app) {
    std::cout << "test_cross_thread_request_stop\n";

    Emitter e; // never emits
    std::stop_source ss;

    QThread * resumedOn = nullptr;
    auto task = awaitVoidCancellableRecordThread85(&e, ss.get_token(), &resumedOn);
    TEST_ASSERT(!task.await_ready(), "should be suspended");

    // Start the worker only after the await is armed — see test 73. The
    // stop callback runs synchronously inside request_stop() on the worker.
    std::unique_ptr<QThread> worker(QThread::create([&ss]() { ss.request_stop(); }));
    worker->start();

    QTimer::singleShot(200, [&]() { app.quit(); });
    app.exec();

    TEST_ASSERT(task.await_ready(), "should resume after cross-thread request_stop");
    if (task.await_ready()) {
        TEST_ASSERT(task.isCancelled(), "should report cancelled");
        TEST_ASSERT(task.cancelReason() == QtCoroutine::AwaitCancelled::Stopped, "reason should be Stopped");
        TEST_ASSERT(resumedOn == QThread::currentThread(), "cancellation unwinds on the awaiting thread");
    }

    worker->wait();
}

// ====================================================================
// 86. Cross-thread request_stop through the cancelledBy(task, token)
//     combinator — the other stop-callback marshalling path
// ====================================================================

QtCoroutine::QTask<int> cancelledByCrossThread86(Emitter * e, std::stop_token st, QThread ** resumedOn) {
    auto task = awaitOneArg(e);
    try {
        co_return co_await QtCoroutine::cancelledBy(task, st);
    } catch (...) {
        *resumedOn = QThread::currentThread();
        throw;
    }
}

void test_cross_thread_request_stop_cancelledBy(QCoreApplication & app) {
    std::cout << "test_cross_thread_request_stop_cancelledBy\n";

    Emitter e; // never emits
    std::stop_source ss;

    QThread * resumedOn = nullptr;
    auto task = cancelledByCrossThread86(&e, ss.get_token(), &resumedOn);
    TEST_ASSERT(!task.await_ready(), "should be suspended");

    std::unique_ptr<QThread> worker(QThread::create([&ss]() { ss.request_stop(); }));
    worker->start(); // after setup — see test 73

    QTimer::singleShot(200, [&]() { app.quit(); });
    app.exec();

    TEST_ASSERT(task.await_ready(), "should resume after cross-thread request_stop");
    if (task.await_ready()) {
        TEST_ASSERT(task.isCancelled(), "should report cancelled");
        TEST_ASSERT(task.cancelReason() == QtCoroutine::AwaitCancelled::Stopped, "reason should be Stopped");
        TEST_ASSERT(resumedOn == QThread::currentThread(), "cancellation unwinds on the awaiting thread");
    }

    worker->wait();
}

// ====================================================================
// 87. QSignalStream — lossless delivery: emissions queued from
//     construction (before the first next()) and a same-thread burst
//     while parked all arrive in order (the fix for the one-shot
//     limitation documented in test 39); the handle stays wired across
//     move construction and move assignment
// ====================================================================

QtCoroutine::QTask<std::vector<int>> streamTake87(QtCoroutine::QSignalStream<int> * s, int count) {
    std::vector<int> out;
    while (static_cast<int>(out.size()) < count) {
        auto v = co_await s->next();
        if (!v)
            break;
        out.push_back(*v);
    }
    co_return out;
}

void test_stream_lossless_burst(QCoreApplication & app) {
    std::cout << "test_stream_lossless_burst\n";

    Emitter e, e2;
    auto s0 = QtCoroutine::stream(&e, &Emitter::oneArg);

    // The connection arms AT CONSTRUCTION: emissions before the first
    // next() are queued, not lost.
    emit e.oneArg(1);
    emit e.oneArg(2);
    emit e.oneArg(3);

    // Moves keep the queue and the connection wired.
    QtCoroutine::QSignalStream<int> s(std::move(s0)); // move ctor
    auto s2 = QtCoroutine::stream(&e2, &Emitter::oneArg);
    s2 = std::move(s);  // move assign disposes the old e2 connection
    emit e2.oneArg(99); // must be ignored — that connection is gone

    auto task = streamTake87(&s2, 6);
    TEST_ASSERT(!task.await_ready(), "consumer drains the pre-next queue and parks for more");

    // Burst while parked: each same-thread emission resumes the consumer
    // synchronously; nothing is lost between resume and the next() re-arm.
    emit e.oneArg(4);
    emit e.oneArg(5);
    emit e.oneArg(6);

    TEST_ASSERT(task.await_ready(), "consumer received all six emissions");
    if (task.await_ready()) {
        auto got = task.await_resume();
        TEST_ASSERT((got == std::vector<int>{1, 2, 3, 4, 5, 6}),
                    "pre-next queue and burst delivered in order, nothing lost");
    }

    QTimer::singleShot(10, [&]() { app.quit(); });
    app.exec();
}

// ====================================================================
// 88. QSignalStream — arity shapes: 0-arg signal -> bool loop ending on
//     sender destruction; 2-arg signal -> optional<tuple>
// ====================================================================

QtCoroutine::QTask<int> streamCountVoid88(QtCoroutine::QSignalStream<> * s) {
    int n = 0;
    while (co_await s->next())
        ++n;
    co_return n;
}

QtCoroutine::QTask<void> streamTakeTwoArgs88(QtCoroutine::QSignalStream<bool, QString> * s, bool * ok, QString * msg) {
    auto v = co_await s->next();
    if (v) {
        *ok = std::get<0>(*v);
        *msg = std::get<1>(*v);
    }
}

void test_stream_arity_shapes(QCoreApplication & app) {
    std::cout << "test_stream_arity_shapes\n";

    // 0-arg signal -> bool: while (co_await s.next()) counts iterations
    // and ends (false) when the sender is destroyed.
    {
        auto * e = new Emitter;
        auto s = QtCoroutine::stream(e, &Emitter::voidSignal);
        auto task = streamCountVoid88(&s);
        TEST_ASSERT(!task.await_ready(), "0-arg consumer parked");

        emit e->voidSignal();
        emit e->voidSignal();
        emit e->voidSignal();
        delete e; // EOF — next() returns false, the loop exits

        TEST_ASSERT(task.await_ready(), "0-arg consumer ended at sender destruction");
        if (task.await_ready())
            TEST_ASSERT(task.await_resume() == 3, "every void emission counted");
    }

    // 2-arg signal -> optional<tuple<bool, QString>>.
    {
        Emitter e;
        static_assert(std::is_same_v<decltype(QtCoroutine::stream(&e, &Emitter::twoArgs)),
                                     QtCoroutine::QSignalStream<bool, QString>>,
                      "stream() deduces the decayed signal argument types");
        auto s = QtCoroutine::stream(&e, &Emitter::twoArgs);
        bool ok = false;
        QString msg;
        auto task = streamTakeTwoArgs88(&s, &ok, &msg);
        TEST_ASSERT(!task.await_ready(), "2-arg consumer parked");

        emit e.twoArgs(true, "stream");

        TEST_ASSERT(task.await_ready(), "2-arg consumer resumed");
        TEST_ASSERT(ok == true, "tuple first element correct");
        TEST_ASSERT(msg == "stream", "tuple second element correct");
    }

    QTimer::singleShot(10, [&]() { app.quit(); });
    app.exec();
}

// ====================================================================
// 89. QSignalStream — latestOnly(): depth-1 conflation keeps only the
//     newest of a burst; a later single emission still arrives normally
// ====================================================================

void test_stream_latestOnly(QCoreApplication & app) {
    std::cout << "test_stream_latestOnly\n";

    Emitter e;
    auto s = QtCoroutine::stream(&e, &Emitter::oneArg).latestOnly();

    // Burst with nobody consuming: each emission replaces the queue.
    emit e.oneArg(1);
    emit e.oneArg(2);
    emit e.oneArg(3);

    auto task = streamTake87(&s, 2);
    TEST_ASSERT(!task.await_ready(), "consumer took the conflated value and parked");

    emit e.oneArg(7); // a later single emission is delivered as usual

    TEST_ASSERT(task.await_ready(), "consumer finished");
    if (task.await_ready()) {
        auto got = task.await_resume();
        TEST_ASSERT((got == std::vector<int>{3, 7}), "only the newest burst value, then the normal one");
    }

    QTimer::singleShot(10, [&]() { app.quit(); });
    app.exec();
}

// ====================================================================
// 90. QSignalStream — sender destroyed: queued values still drain, then
//     next() returns nullopt forever (idempotent EOF, no exception)
// ====================================================================

QtCoroutine::QTask<std::vector<int>> streamDrainToEof90(QtCoroutine::QSignalStream<int> * s, int * eofCount) {
    std::vector<int> out;
    while (auto v = co_await s->next())
        out.push_back(*v);
    ++*eofCount;
    if (!(co_await s->next())) // EOF must be idempotent
        ++*eofCount;
    co_return out;
}

void test_stream_sender_destroyed_drains(QCoreApplication & app) {
    std::cout << "test_stream_sender_destroyed_drains\n";

    auto * e = new Emitter;
    auto s = QtCoroutine::stream(e, &Emitter::oneArg);

    emit e->oneArg(10);
    emit e->oneArg(20);
    delete e; // the queue still holds both values

    int eofCount = 0;
    auto task = streamDrainToEof90(&s, &eofCount);

    TEST_ASSERT(task.await_ready(), "drain and EOF all settle synchronously");
    if (task.await_ready()) {
        auto got = task.await_resume();
        TEST_ASSERT((got == std::vector<int>{10, 20}), "queued values delivered after sender death");
    }
    TEST_ASSERT(eofCount == 2, "next() keeps returning nullopt after EOF — idempotent, no exception");

    QTimer::singleShot(10, [&]() { app.quit(); });
    app.exec();
}

// ====================================================================
// 91. QSignalStream — cancelledBy(): the stop is terminal and immediate
//     (queued values are NOT drained); a pre-stopped token behaves the
//     same; request_stop may come from any thread
// ====================================================================

QtCoroutine::QTask<void> streamConsumeCancellable91(QtCoroutine::QSignalStream<int> * s, int * delivered,
                                                    int * stoppedThrows) {
    try {
        while (co_await s->next())
            ++*delivered;
    } catch (const QtCoroutine::AwaitCancelled & c) {
        if (c.wasStopped())
            ++*stoppedThrows;
    }
    try {
        co_await s->next(); // terminal: every later next() throws too
    } catch (const QtCoroutine::AwaitCancelled & c) {
        if (c.wasStopped())
            ++*stoppedThrows;
    }
}

void test_stream_cancelledBy(QCoreApplication & app) {
    std::cout << "test_stream_cancelledBy\n";

    // (a) Pending next(): request_stop wakes it with AwaitCancelled{Stopped}.
    {
        Emitter e;
        std::stop_source ss;
        auto s = QtCoroutine::stream(&e, &Emitter::oneArg).cancelledBy(ss.get_token());

        int delivered = 0, stoppedThrows = 0;
        auto task = streamConsumeCancellable91(&s, &delivered, &stoppedThrows);
        TEST_ASSERT(!task.await_ready(), "consumer parked");

        emit e.oneArg(1); // still flows before the stop
        TEST_ASSERT(delivered == 1, "value delivered before the stop");

        ss.request_stop(); // same-thread: wakes the parked next() synchronously

        TEST_ASSERT(task.await_ready(), "consumer unwound after the stop");
        TEST_ASSERT(stoppedThrows == 2, "pending next() and the later next() both threw Stopped");
    }

    // (b) Queued values must NOT be drained once the token fired.
    {
        Emitter e;
        std::stop_source ss;
        auto s = QtCoroutine::stream(&e, &Emitter::oneArg).cancelledBy(ss.get_token());

        emit e.oneArg(1);
        emit e.oneArg(2);
        ss.request_stop();

        int delivered = 0, stoppedThrows = 0;
        auto task = streamConsumeCancellable91(&s, &delivered, &stoppedThrows);
        TEST_ASSERT(task.await_ready(), "stopped stream settles next() immediately");
        TEST_ASSERT(delivered == 0, "stop wins over queued values — no drain");
        TEST_ASSERT(stoppedThrows == 2, "first and subsequent next() throw Stopped");
    }

    // (c) Token already stopped at builder time behaves the same.
    {
        Emitter e;
        std::stop_source ss;
        ss.request_stop();
        auto s = QtCoroutine::stream(&e, &Emitter::oneArg).cancelledBy(ss.get_token());
        emit e.oneArg(5); // ignored — the stream is already stopped

        int delivered = 0, stoppedThrows = 0;
        auto task = streamConsumeCancellable91(&s, &delivered, &stoppedThrows);
        TEST_ASSERT(task.await_ready(), "pre-stopped token settles next() immediately");
        TEST_ASSERT(delivered == 0 && stoppedThrows == 2, "pre-stopped token behaves like a later stop");
    }

    // (d) request_stop from another thread marshals the wake back to the
    //     consumer thread.
    {
        Emitter e; // never emits
        std::stop_source ss;
        auto s = QtCoroutine::stream(&e, &Emitter::oneArg).cancelledBy(ss.get_token());

        int delivered = 0, stoppedThrows = 0;
        auto task = streamConsumeCancellable91(&s, &delivered, &stoppedThrows);
        TEST_ASSERT(!task.await_ready(), "consumer parked");

        std::unique_ptr<QThread> worker(QThread::create([&ss]() { ss.request_stop(); }));
        worker->start(); // after setup — see test 73

        QTimer::singleShot(200, [&]() { app.quit(); });
        app.exec();
        worker->wait();

        TEST_ASSERT(task.await_ready(), "cross-thread request_stop cancels the pending next()");
        TEST_ASSERT(delivered == 0 && stoppedThrows == 2, "stop delivered on the consumer thread");
    }
}

// ====================================================================
// 92. QSignalStream — withTimeout(): a queued value returns immediately
//     (no timeout while not waiting); a silent wait throws
//     AwaitCancelled{Timeout}; the stream stays armed afterwards
// ====================================================================

QtCoroutine::QTask<void> streamTimeoutRecover92(QtCoroutine::QSignalStream<int> * s, std::vector<int> * got,
                                                int * timeouts) {
    if (auto v = co_await s->next()) // queued value: no wait, no timeout
        got->push_back(*v);
    try {
        co_await s->next(); // nothing queued, nothing emitted -> Timeout
    } catch (const QtCoroutine::AwaitCancelled & c) {
        if (c.wasTimedOut())
            ++*timeouts;
    }
    if (auto v = co_await s->next()) // non-fatal: still armed and usable
        got->push_back(*v);
}

void test_stream_withTimeout(QCoreApplication & app) {
    std::cout << "test_stream_withTimeout\n";

    Emitter e;
    auto s = QtCoroutine::stream(&e, &Emitter::oneArg).withTimeout(std::chrono::milliseconds(150));

    emit e.oneArg(1); // queued before the consumer starts

    std::vector<int> got;
    int timeouts = 0;
    auto task = streamTimeoutRecover92(&s, &got, &timeouts);
    TEST_ASSERT(!task.await_ready(), "consumer parked on the waiting next()");
    TEST_ASSERT((got == std::vector<int>{1}), "queued value returned immediately, no timeout");

    // The parked next() times out at ~150ms. Emit at 225ms: after that
    // timeout, well before the recovered next()'s own ~300ms deadline.
    QTimer::singleShot(225, [&]() { emit e.oneArg(2); });

    QTimer::singleShot(600, [&]() {
        TEST_ASSERT(task.await_ready(), "consumer finished");
        TEST_ASSERT(timeouts == 1, "silent wait threw AwaitCancelled{Timeout}");
        TEST_ASSERT((got == std::vector<int>{1, 2}), "stream stayed armed after the timeout (non-fatal)");
        app.quit();
    });

    app.exec();
}

// ====================================================================
// 93. QSignalStream — cross-thread lossless: a rapid worker-thread burst
//     of 50 values is fully delivered, in order, with the loop body on
//     the consumer (main) thread
// ====================================================================

QtCoroutine::QTask<void> streamConsumeOnThread93(QtCoroutine::QSignalStream<int> * s, std::vector<int> * out,
                                                 std::atomic<bool> * threadOk, QThread * expected,
                                                 std::atomic<bool> * done, int count) {
    while (static_cast<int>(out->size()) < count) {
        auto v = co_await s->next();
        if (!v)
            break;
        if (QThread::currentThread() != expected)
            threadOk->store(false, std::memory_order_relaxed);
        out->push_back(*v);
    }
    done->store(true, std::memory_order_release);
}

void test_stream_cross_thread_lossless(QCoreApplication & app) {
    std::cout << "test_stream_cross_thread_lossless\n";

    constexpr int N = 50;

    Emitter e; // lives on the main thread; emissions are inputs from anywhere
    auto s = QtCoroutine::stream(&e, &Emitter::oneArg);

    std::vector<int> got;
    std::atomic<bool> threadOk{true};
    std::atomic<bool> done{false};
    auto task = streamConsumeOnThread93(&s, &got, &threadOk, QThread::currentThread(), &done, N);
    TEST_ASSERT(!task.await_ready(), "consumer parked before the burst");

    task.then([&]() { app.quit(); }); // completes on the consumer (main) thread

    // Start the worker only after stream + consumer setup — see test 73.
    std::unique_ptr<QThread> worker(QThread::create([&e]() {
        for (int i = 0; i < N; ++i)
            emit e.oneArg(i); // enqueued directly from the emitting thread
    }));
    worker->start();

    QTimer::singleShot(2000, [&]() { app.quit(); }); // watchdog
    app.exec();
    worker->wait();

    TEST_ASSERT(task.await_ready(), "consumer received the full burst");
    TEST_ASSERT(static_cast<int>(got.size()) == N, "all 50 cross-thread emissions delivered (lossless)");
    bool ordered = got.size() == static_cast<std::size_t>(N);
    for (int i = 0; i < static_cast<int>(got.size()); ++i)
        if (got[i] != i)
            ordered = false;
    TEST_ASSERT(ordered, "values delivered in emission order");
    TEST_ASSERT(threadOk.load(), "loop body ran on the consumer (main) thread");
}

// ====================================================================
// 94. QSignalStream — same-thread synchronous resume (direct-slot
//     semantics) and re-entrancy: emissions from inside the loop body
//     are queued and delivered by subsequent next() calls
// ====================================================================

QtCoroutine::QTask<void> streamReentrantConsumer94(QtCoroutine::QSignalStream<int> * s, Emitter * e,
                                                   std::vector<int> * got) {
    while (auto v = co_await s->next()) {
        got->push_back(*v);
        if (*v == 1) {
            // We are running INSIDE the test's emit statement. These nested
            // emissions find no parked waiter and must queue, not vanish.
            emit e->oneArg(2);
            emit e->oneArg(3);
        }
        if (*v == 3)
            break;
    }
}

void test_stream_synchronous_resume_reentrancy(QCoreApplication & app) {
    std::cout << "test_stream_synchronous_resume_reentrancy\n";

    Emitter e;
    auto s = QtCoroutine::stream(&e, &Emitter::oneArg);

    std::vector<int> got;
    auto task = streamReentrantConsumer94(&s, &e, &got);
    TEST_ASSERT(!task.await_ready(), "consumer parked");

    emit e.oneArg(1); // resumes the consumer synchronously, like a direct slot

    // Everything happened inside the emit statement above: the loop body ran
    // synchronously and its nested emissions were consumed without loss.
    TEST_ASSERT((got == std::vector<int>{1, 2, 3}),
                "loop body ran inside the emit; nested emissions queued and delivered in order");
    TEST_ASSERT(task.await_ready(), "consumer completed inside the emit");

    QTimer::singleShot(10, [&]() { app.quit(); });
    app.exec();
}

// ====================================================================
// 95. QSignalStream — coroutine frame destroyed while parked on next():
//     a later emission must not resume freed memory (ASan-verified); it
//     is safely queued and the stream stays usable
// ====================================================================

QtCoroutine::QTask<void> streamParkedConsumer95(QtCoroutine::QSignalStream<int> * s, bool * resumed) {
    co_await s->next();
    *resumed = true;
}

void test_stream_frame_destroyed_while_parked(QCoreApplication & app) {
    std::cout << "test_stream_frame_destroyed_while_parked\n";

    Emitter e;
    auto s = QtCoroutine::stream(&e, &Emitter::oneArg);

    bool resumed = false;
    {
        auto task = streamParkedConsumer95(&s, &resumed);
        TEST_ASSERT(!task.await_ready(), "consumer parked");
    } // QTask destroyed: frame destruction unparks the pending next()

    emit e.oneArg(7); // must not touch the destroyed frame

    TEST_ASSERT(!resumed, "destroyed frame must not resume");

    // The stream is still healthy: a fresh consumer picks up the value the
    // dropped wake left queued.
    auto task2 = streamTake87(&s, 1);
    TEST_ASSERT(task2.await_ready(), "fresh consumer drained the safely-queued emission");
    if (task2.await_ready())
        TEST_ASSERT((task2.await_resume() == std::vector<int>{7}), "the emission was queued, not lost");

    QTimer::singleShot(10, [&]() { app.quit(); });
    app.exec();
}

// ====================================================================
// 96. QSignalStream — next().asExpected(): values pass through; after
//     request_stop every await yields unexpected(wasStopped) — no throw
// ====================================================================

QtCoroutine::QTask<void> streamAsExpected96(QtCoroutine::QSignalStream<int> * s, std::vector<int> * got,
                                            int * stoppedErrors) {
    auto ok = co_await s->next().asExpected();
    if (ok && *ok)
        got->push_back(**ok);
    auto r1 = co_await s->next().asExpected(); // the stop arrives while parked
    if (!r1 && r1.error().wasStopped())
        ++*stoppedErrors;
    auto r2 = co_await s->next().asExpected(); // terminal, still no throw
    if (!r2 && r2.error().wasStopped())
        ++*stoppedErrors;
}

void test_stream_asExpected(QCoreApplication & app) {
    std::cout << "test_stream_asExpected\n";

    Emitter e;
    std::stop_source ss;
    auto s = QtCoroutine::stream(&e, &Emitter::oneArg).cancelledBy(ss.get_token());

    std::vector<int> got;
    int stoppedErrors = 0;
    auto task = streamAsExpected96(&s, &got, &stoppedErrors);
    TEST_ASSERT(!task.await_ready(), "consumer parked on the first next()");

    emit e.oneArg(9); // success path through asExpected
    TEST_ASSERT((got == std::vector<int>{9}), "asExpected passes values through");

    ss.request_stop(); // resumes the parked next() synchronously

    TEST_ASSERT(task.await_ready(), "consumer ran to completion — nothing thrown");
    TEST_ASSERT(stoppedErrors == 2, "stop surfaces as unexpected(wasStopped) on this and every later next()");
    TEST_ASSERT(!task.isCancelled(), "no AwaitCancelled escaped the coroutine");

    QTimer::singleShot(10, [&]() { app.quit(); });
    app.exec();
}

// ====================================================================
// 97. QSignalStream — resumeOn(ctx) PINS the consumer loop to ctx's
//     thread: next() is awaited on the worker from the first call,
//     deliveries land there, and main-thread emissions marshal across
//     (infrastructure mirrors test 76)
// ====================================================================

void test_stream_resumeOn_pins_consumer_thread(QCoreApplication & app) {
    std::cout << "test_stream_resumeOn_pins_consumer_thread\n";

    QThread worker;
    worker.start();
    while (!worker.eventDispatcher()) // the pinned thread needs a live dispatcher
        QThread::msleep(1);

    Emitter e; // sender stays on the main thread
    QObject ctx;
    ctx.moveToThread(&worker);

    // Built on main; consumption is pinned to ctx's (the worker's) thread.
    auto s = QtCoroutine::stream(&e, &Emitter::oneArg).resumeOn(&ctx);

    std::vector<int> got;
    std::atomic<bool> threadOk{true};
    std::atomic<bool> done{false};

    // Start the consumer ON the worker thread (pinning is not migration:
    // the first next() must already be awaited there). The detached frame
    // self-destructs on completion.
    auto * sp = &s;
    QMetaObject::invokeMethod(
        &ctx,
        [sp, &got, &threadOk, &worker, &done]() {
            streamConsumeOnThread93(sp, &got, &threadOk, &worker, &done, 3).detach();
        },
        Qt::QueuedConnection);

    QTimer::singleShot(50, [&]() {
        emit e.oneArg(1); // main-thread emissions, marshalled to the pinned worker
        emit e.oneArg(2);
        emit e.oneArg(3);
    });

    QTimer pump; // poll for completion instead of guessing a fixed delay
    QObject::connect(&pump, &QTimer::timeout, [&]() {
        if (done.load(std::memory_order_acquire))
            app.quit();
    });
    pump.start(10);
    QTimer::singleShot(2000, [&]() { app.quit(); }); // watchdog
    app.exec();

    worker.quit();
    worker.wait(); // join: publishes `got` back to the main thread

    TEST_ASSERT(done.load(std::memory_order_acquire), "pinned consumer completed on the worker");
    TEST_ASSERT(threadOk.load(), "loop body observed the resumeOn ctx's thread");
    TEST_ASSERT((got == std::vector<int>{1, 2, 3}), "all values delivered to the pinned thread, in order");
}

int main(int argc, char * argv[]) {
    QCoreApplication app(argc, argv);

    // Synchronous tests (no event loop needed)
    test_qtask_basic();
    test_qtask_move();
    test_qtask_then();
    test_readyIf_void();
    test_readyIf_two_args();
    test_exception_propagation();
    test_onError_fires();
    test_qtask_void_move_assignment();
    test_whenAll_sync();
    test_whenAll_void();
    test_whenAny_immediate();
    test_qtask_expected_void();
    test_withTimeout_already_complete();
    test_detach_already_done();
    test_precancelled_stop_token();
    test_qfuture_already_failed();
    test_whenAll_awaiter_destroyed();
    test_whenAll_void_awaiter_destroyed();
    test_whenAny_awaiter_destroyed();
    test_reassign_task_inside_callback();
    test_detach_inside_callback();
    test_waitFor();
    test_whenAny_void_index();
    test_whenAny_void_cancelled();
    test_whenAny_variant();
    test_whenAny_variant_error();
    test_whenAny_void_awaiter_destroyed();
    test_whenAny_variant_awaiter_destroyed();
    test_hygiene_api();

    // Async tests (need event loop)
    test_signal_void(app);
    test_signal_one_arg(app);
    test_signal_two_args(app);
    test_cancellation(app);
    test_resume_on(app);
    test_sender_destroyed(app);
    test_qfuture_await(app);
    test_qfuture_chained_concurrent(app);
    test_qtask_cancellation_state(app);
    test_readyIf_one_arg(app);
    test_qfuture_void(app);
    test_sleep(app);
    test_withTimeout_success(app);
    test_withTimeout_expired(app);
    test_asExpected_success(app);
    test_asExpected_cancelled(app);
    test_asExpected_void(app);
    test_onCancelled_fires(app);
    test_onCancelled_fires_immediately(app);
    test_onCancelled_void_task(app);
    test_toFuture_success(app);
    test_toFuture_cancellation(app);
    test_toFuture_void(app);
    test_whenAll_async(app);
    test_whenAny_first_wins(app);
    test_qtask_destroyed_while_suspended(app);
    test_resume_context_destroyed(app);
    test_signal_before_coawait(app);
    test_multiple_emissions(app);
    test_whenAll_one_cancelled(app);
    test_whenAll_one_errors(app);
    test_whenAll_mixed_timing(app);
    test_tryAll_short_circuits(app);
    test_toFuture_exception(app);
    test_timeout_and_cancel_together(app);
    test_cancelledBy_success(app);
    test_cancelledBy_stopped(app);
    test_cancelledBy_asExpected(app);
    test_withTimeout_qtask_success(app);
    test_withTimeout_qtask_expired(app);
    test_withTimeout_qtask_expected(app);
    test_withTimeout_qfuture_success(app);
    test_withTimeout_qfuture_expired(app);
    test_withTimeout_qfuture_in_future(app);
    test_cancelledBy_qfuture_success(app);
    test_detach_inflight(app);
    test_detach_no_callback(app);
    test_detach_onCancelled(app);
    test_detach_onError(app);
    test_detach_double(app);
    test_detach_regression_nondetached(app);
    test_detach_regression_coawait(app);
    test_qfuture_exception_while_suspended(app);
    test_qfuture_exception_from_worker(app);
    test_qfuture_cancel_while_suspended(app);
    test_stop_then_destroy_before_resume(app);
    test_sleep_destroyed_while_suspended(app);
    test_withTimeout_readyIf_order(app);
    test_cross_thread_sender_resumes_on_awaiting_thread(app);
    test_cross_thread_sender_destroyed(app);
    test_cross_thread_sender_timeout(app);
    test_resumeOn_migrates_to_ctx_thread(app);
    test_then_with_cross_thread_completion(app);
    test_cross_thread_request_stop(app);
    test_cross_thread_request_stop_cancelledBy(app);
    test_stream_lossless_burst(app);
    test_stream_arity_shapes(app);
    test_stream_latestOnly(app);
    test_stream_sender_destroyed_drains(app);
    test_stream_cancelledBy(app);
    test_stream_withTimeout(app);
    test_stream_cross_thread_lossless(app);
    test_stream_synchronous_resume_reentrancy(app);
    test_stream_frame_destroyed_while_parked(app);
    test_stream_asExpected(app);
    test_stream_resumeOn_pins_consumer_thread(app);

    std::cout << "\n" << g_passed << " passed, " << g_failed << " failed\n";
    return g_failed > 0 ? 1 : 0;
}

#include "test_core.moc"
