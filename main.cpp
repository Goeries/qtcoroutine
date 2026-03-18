#include <expected>
#include <QCoreApplication>
#include <QtConcurrent>
#include "include/qtcoroutine/qtcoroutine.hpp"
#include "include/qtcoroutine/qfuture_coroutine_traits.hpp"
#include "include/qtcoroutine/qtask.hpp"

class Derived : public QObject {
    Q_OBJECT

public:
    explicit Derived(QObject * parent = nullptr) : QObject(parent) {};

    void startAsyncTask(std::stop_token stopToken = {}) {
        auto f = QtConcurrent::run([this](){
            QThread::sleep(3000);   // Simulate work
            m_hasActiveTask = false;
        });

        m_hasActiveTask = true;
    }

    bool hasActiveTask() const { return m_hasActiveTask; }

    void cancelTask() const;

signals:
    void asyncTaskFinished(bool success, const QByteArray & output);

private:
    bool m_hasActiveTask = false;
};

int heavyComputation() { return 1; }
int transformation(int input) { return input * 2; }
int evenHeavierComputation(int input) { return input + 10; }

QtCoroutine::QTask<std::expected<QByteArray, QString>> signalExample(std::stop_token stopToken = {}) {
    Derived obj;  // Local variable is ok here: will persist until coroutine handle cleans up
    obj.startAsyncTask();

    auto [success, output] = co_await QtCoroutine::signal(&obj, &Derived::asyncTaskFinished)
                                 .resumeOn(&obj)
                                 .cancelledBy(stopToken);

    if (success) co_return output;
    co_return std::unexpected{"Error occurred"};
}

QFuture<int> futureExample(std::stop_token stopToken = {}) {
    auto future = QtConcurrent::run([]() {
        return heavyComputation();
    });

    auto value = co_await future;   // Requires a running event loop on this thread (QThread::currentThread()->eventDispatcher()) to resume and execute code below back on this thread
    value = transformation(value);

    value = co_await QtConcurrent::run([](int input) {
        return evenHeavierComputation(input);
    }, value);

    co_return value;
}

QtCoroutine::QTask<std::pair<QByteArray, int>> coroutineExample(std::stop_token stopToken = {}) {
    auto task = signalExample(stopToken);
    auto taskResult = co_await task;

    auto future = futureExample(stopToken);
    auto futureResult = co_await future;

    co_return {
        taskResult ? *taskResult : QByteArray{},
        futureResult
    };
}

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv); // Needed for proper coroutine resumption

    std::stop_source stopSource;
    auto task = coroutineExample(stopSource.get_token());

    // Use continuations from non-coroutine code
    task.then([](const std::pair<QByteArray, int> & result) {
        qInfo() << "signalExample() result:" << result.first;
        qInfo() << "futureExample() result:" << result.second;
    });

    return a.exec();
}

#include "main.moc"
