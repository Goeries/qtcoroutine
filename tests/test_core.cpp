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

QtCoroutine::QTask<int> whenAnyTest34(Emitter * e1, Emitter * e2) {
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
        TEST_ASSERT(task.await_resume() == 55, "winning task result correct");
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
    TEST_ASSERT(awaitable.await_resume() == 42, "result should be 42");
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

    std::cout << "\n" << g_passed << " passed, " << g_failed << " failed\n";
    return g_failed > 0 ? 1 : 0;
}

#include "test_core.moc"
