// Minimal QtCoroutine example.
//
// Shows the three core pieces working together — a QTask coroutine that
// co_awaits a QFuture (background work) and a Qt signal (a timer) — then
// reports the result from ordinary callback code via .then().
//
// It is built (and run) in CI so these snippets can't silently rot.

#include <QCoreApplication>
#include <QTimer>
#include <QtConcurrent>

#include <chrono>
#include <iostream>

#include <QtCoroutine>

QtCoroutine::QTask<int> sumOfSquares() {
    int sum = 0;

    // co_await a QFuture<int> produced on a worker thread.
    for (int i = 0; i < 5; ++i)
        sum += co_await QtConcurrent::run([i]() { return i * i; });

    // co_await a Qt signal — QTimer::timeout needs no custom QObject.
    QTimer timer;
    timer.setSingleShot(true);
    timer.start(10);
    co_await QtCoroutine::connect(&timer, &QTimer::timeout);

    co_return sum;
}

int main(int argc, char ** argv) {
    QCoreApplication app(argc, argv);

    int exitCode = 0;
    auto task = sumOfSquares();
    task.then([&](int result) {
        std::cout << "sum of squares 0..4 = " << result << '\n'; // 0+1+4+9+16 = 30
        app.quit();
    });

    // Safety net so the example can never hang CI.
    QTimer::singleShot(std::chrono::seconds(10), &app, [&]() {
        std::cerr << "example timed out\n";
        exitCode = 1;
        app.quit();
    });

    app.exec();
    return exitCode;
}
