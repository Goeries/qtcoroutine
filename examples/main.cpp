#include <QCoreApplication>
#include <QtConcurrent>
#include <QDebug>
#include <QTimer>
#include <qtcoroutine/qfuture_coroutine.hpp>
#include <qtcoroutine/qtcoroutine.hpp>

using namespace QtCoroutine;

// --- QFuture as coroutine return type and awaitable ---

QFuture<int> computeWithQFuture() {
    int value = co_await QtConcurrent::run([] { return 21; });
    co_return value * 2;
}

QFuture<void> doWorkWithQFuture() {
    co_await QtConcurrent::run([] { qDebug() << "QFuture<void> side effect"; });
    co_return;
}

// --- QTask as coroutine return type and awaitable ---

QTask<int> computeWithQTask() {
    auto task = QTask<int>::fromFuture(QtConcurrent::run([] { return 21; }));
    int value = co_await task;
    co_return value * 2;
}

QTask<void> doWorkWithQTask() {
    auto task = QTask<void>::fromFuture(QtConcurrent::run([] {
        qDebug() << "QTask<void> side effect";
    }));
    co_await task;
    co_return;
}

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    // QFuture examples
    auto futureInt = computeWithQFuture();
    futureInt.waitForFinished();
    qDebug() << "QFuture<int> result:" << futureInt.result();

    auto futureVoid = doWorkWithQFuture();
    futureVoid.waitForFinished();
    qDebug() << "QFuture<void> finished";

    // QTask examples (using blocking result() / waitForFinished())
    auto taskInt = computeWithQTask();
    qDebug() << "QTask<int> result:" << taskInt.result();

    auto taskVoid = doWorkWithQTask();
    taskVoid.waitForFinished();
    qDebug() << "QTask<void> finished";

    QTimer::singleShot(0, &a, &QCoreApplication::quit);
    return a.exec();
}
