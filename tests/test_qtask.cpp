#include <QtTest>
#include <QtConcurrent>
#include <qtcoroutine/qtcoroutine.hpp>

using namespace QtCoroutine;

// Spins the event loop until the task finishes (needed for QFutureWatcher signals)
template<typename T>
void waitForTask(QTask<T> &task) {
    while (!task.isFinished())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

class TestQTask : public QObject
{
    Q_OBJECT

private:
    // -- QTask<T> as coroutine return type --

    QTask<int> coroutineReturningInt() {
        co_return 42;
    }

    QTask<QString> coroutineReturningString() {
        co_return QStringLiteral("hello");
    }

    // -- QTask<void> as coroutine return type --

    QTask<void> coroutineReturningVoid() {
        co_return;
    }

    // -- QTask<T> as awaitable (await a QTask from within a QTask) --

    QTask<int> innerTask() {
        co_return 10;
    }

    QTask<int> awaitTaskInt() {
        int result = co_await innerTask();
        co_return result + 5;
    }

    QTask<QString> innerStringTask() {
        co_return QStringLiteral("world");
    }

    QTask<QString> awaitTaskString() {
        QString result = co_await innerStringTask();
        co_return QStringLiteral("hello ") + result;
    }

    // -- QTask<void> as awaitable --

    QTask<void> innerVoidTask() {
        co_return;
    }

    QTask<void> awaitTaskVoid() {
        co_await innerVoidTask();
        co_return;
    }

    // -- Exception propagation --

    QTask<int> awaitThrowingConcurrent() {
        auto task = QTask<int>::fromFuture(
            QtConcurrent::run([]() -> int {
                throw std::runtime_error("concurrent error");
            })
        );
        int result = co_await task;
        co_return result;
    }

    QTask<int> coroutineThrowingDirectly() {
        throw std::runtime_error("coroutine error");
        co_return 0;
    }

    QTask<void> awaitThrowingVoid() {
        auto task = QTask<void>::fromFuture(
            QtConcurrent::run([] {
                throw std::logic_error("void concurrent error");
            })
        );
        co_await task;
        co_return;
    }

    // -- Chained awaits --

    QTask<int> chainedAwait() {
        int a = co_await innerTask();
        int b = co_await QTask<int>::fromFuture(
            QtConcurrent::run([a] { return a * 2; })
        );
        co_return b + 1;
    }

private slots:
    void testCoroutineReturnInt() {
        auto task = coroutineReturningInt();
        waitForTask(task);
        QCOMPARE(task.result(), 42);
    }

    void testCoroutineReturnString() {
        auto task = coroutineReturningString();
        waitForTask(task);
        QCOMPARE(task.result(), QStringLiteral("hello"));
    }

    void testCoroutineReturnVoid() {
        auto task = coroutineReturningVoid();
        waitForTask(task);
        QVERIFY(task.isFinished());
    }

    void testAwaitTaskInt() {
        auto task = awaitTaskInt();
        waitForTask(task);
        QCOMPARE(task.result(), 15);
    }

    void testAwaitTaskString() {
        auto task = awaitTaskString();
        waitForTask(task);
        QCOMPARE(task.result(), QStringLiteral("hello world"));
    }

    void testAwaitTaskVoid() {
        auto task = awaitTaskVoid();
        waitForTask(task);
        QVERIFY(task.isFinished());
    }

    void testExceptionFromConcurrent() {
        auto task = awaitThrowingConcurrent();
        waitForTask(task);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, (void)task.result());
    }

    void testExceptionFromCoroutine() {
        auto task = coroutineThrowingDirectly();
        waitForTask(task);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, (void)task.result());
    }

    void testExceptionFromVoidConcurrent() {
        auto task = awaitThrowingVoid();
        waitForTask(task);
        QVERIFY_THROWS_EXCEPTION(std::logic_error, task.waitForFinished());
    }

    void testChainedAwait() {
        auto task = chainedAwait();
        waitForTask(task);
        QCOMPARE(task.result(), 21);
    }
};

QTEST_MAIN(TestQTask)
#include "test_qtask.moc"
