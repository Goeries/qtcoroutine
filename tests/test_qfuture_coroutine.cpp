#include <QtTest>
#include <QtConcurrent>
#include <qtcoroutine/qfuture_coroutine.hpp>

// Spins the event loop until the future finishes (needed for QFutureWatcher signals)
template<typename T>
void waitForFuture(QFuture<T> &future) {
    while (!future.isFinished())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

class TestQFutureCoroutine : public QObject
{
    Q_OBJECT

private:
    // -- QFuture<T> as coroutine return type --

    QFuture<int> coroutineReturningInt() {
        co_return 42;
    }

    QFuture<QString> coroutineReturningString() {
        co_return QStringLiteral("hello");
    }

    // -- QFuture<void> as coroutine return type --

    QFuture<void> coroutineReturningVoid() {
        co_return;
    }

    // -- QFuture<T> as awaitable --

    QFuture<int> awaitConcurrentInt() {
        int result = co_await QtConcurrent::run([] { return 10; });
        co_return result + 5;
    }

    QFuture<QString> awaitConcurrentString() {
        QString result = co_await QtConcurrent::run([] {
            return QStringLiteral("world");
        });
        co_return QStringLiteral("hello ") + result;
    }

    // -- QFuture<void> as awaitable --

    QFuture<void> awaitConcurrentVoid() {
        co_await QtConcurrent::run([] { /* side effect */ });
        co_return;
    }

    // -- Exception propagation --

    QFuture<int> awaitThrowingConcurrent() {
        int result = co_await QtConcurrent::run([]() -> int {
            throw std::runtime_error("concurrent error");
        });
        co_return result;
    }

    QFuture<int> coroutineThrowingDirectly() {
        throw std::runtime_error("coroutine error");
        co_return 0;
    }

    QFuture<void> awaitThrowingVoid() {
        co_await QtConcurrent::run([] {
            throw std::logic_error("void concurrent error");
        });
        co_return;
    }

    // -- Chained awaits --

    QFuture<int> chainedAwait() {
        int a = co_await QtConcurrent::run([] { return 10; });
        int b = co_await QtConcurrent::run([a] { return a * 2; });
        co_return b + 1;
    }

private slots:
    void testCoroutineReturnInt() {
        auto future = coroutineReturningInt();
        waitForFuture(future);
        QCOMPARE(future.result(), 42);
    }

    void testCoroutineReturnString() {
        auto future = coroutineReturningString();
        waitForFuture(future);
        QCOMPARE(future.result(), QStringLiteral("hello"));
    }

    void testCoroutineReturnVoid() {
        auto future = coroutineReturningVoid();
        waitForFuture(future);
        QVERIFY(future.isFinished());
    }

    void testAwaitInt() {
        auto future = awaitConcurrentInt();
        waitForFuture(future);
        QCOMPARE(future.result(), 15);
    }

    void testAwaitString() {
        auto future = awaitConcurrentString();
        waitForFuture(future);
        QCOMPARE(future.result(), QStringLiteral("hello world"));
    }

    void testAwaitVoid() {
        auto future = awaitConcurrentVoid();
        waitForFuture(future);
        QVERIFY(future.isFinished());
    }

    void testExceptionFromConcurrent() {
        auto future = awaitThrowingConcurrent();
        waitForFuture(future);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, future.result());
    }

    void testExceptionFromCoroutine() {
        auto future = coroutineThrowingDirectly();
        waitForFuture(future);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, future.result());
    }

    void testExceptionFromVoidConcurrent() {
        auto future = awaitThrowingVoid();
        waitForFuture(future);
        QVERIFY_THROWS_EXCEPTION(std::logic_error, future.waitForFinished());
    }

    void testChainedAwait() {
        auto future = chainedAwait();
        waitForFuture(future);
        QCOMPARE(future.result(), 21);
    }
};

QTEST_MAIN(TestQFutureCoroutine)
#include "test_qfuture_coroutine.moc"
