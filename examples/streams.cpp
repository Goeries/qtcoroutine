// Signal streams: lossless co_await loops over repeating signals — counting
// ticks, conflating progress updates, surviving per-wait timeouts, and ending
// naturally when the sender is destroyed (EOF, no exception).
//
// It is built (and run) in CI so these snippets can't silently rot.

#include <QCoreApplication>
#include <QTimeLine>
#include <QTimer>

#include <chrono>
#include <iostream>
#include <stop_token>

#include <QtCoroutine>

using namespace std::chrono_literals;

// 1. Count every tick until the sender dies — sender destruction is EOF:
//    queued values drain, then next() returns false and the loop falls
//    through. The stream is moved into the coroutine frame: handles store
//    and move like any value (QSignalStream<> is the 0-arg shape; the
//    stream() factory deduces it from the signal).
QtCoroutine::QTask<int> countTicks(QtCoroutine::QSignalStream<> ticks) {
    int n = 0;
    while (co_await ticks.next())
        ++n;
    co_return n;
}

// 2. Progress conflation: the consumer is slower than the producer, and
//    .latestOnly() keeps only the newest frame — stale intermediates are
//    dropped instead of queueing up behind the slow handler.
QtCoroutine::QTask<void> watchProgress(QtCoroutine::QSignalStream<int> frames) {
    while (auto frame = co_await frames.next()) {
        std::cout << "progress: frame " << *frame << '\n';
        co_await QtCoroutine::sleep(50ms); // simulate slow handling
    }
}

// 3. Per-next inactivity timeout, non-fatal: a wait that outlasts the
//    timeout throws AwaitCancelled{Timeout}, but the stream stays armed —
//    the next await still delivers the late tick.
QtCoroutine::QTask<void> impatientTick(QtCoroutine::QSignalStream<> ticks) {
    try {
        co_await ticks.next();
    } catch (const QtCoroutine::AwaitCancelled & c) {
        std::cout << "timeout: " << c.message() << '\n';
    }
    if (co_await ticks.next()) // same stream, still usable
        std::cout << "timeout: late tick arrived on the second wait\n";
}

// 4. Cooperative cancellation, value-based: .cancelledBy(st) aborts a
//    pending next() with Stopped, and .asExpected() reports it as a value
//    instead of an exception.
QtCoroutine::QTask<void> waitForTickUntilStopped(QtCoroutine::QSignalStream<> ticks) {
    auto tick = co_await ticks.next().asExpected();
    if (!tick)
        std::cout << "cancelled: " << tick.error().message() << '\n';
}

int main(int argc, char ** argv) {
    QCoreApplication app(argc, argv);

    // 1. Tick until the sender is destroyed.
    {
        auto * timer = new QTimer;
        timer->setInterval(10ms);
        auto ticks = QtCoroutine::stream(timer, &QTimer::timeout);
        timer->start();
        QTimer::singleShot(75ms, [timer]() { delete timer; }); // EOF for the stream
        std::cout << "ticks before sender died: " << QtCoroutine::waitFor(countTicks(std::move(ticks))) << '\n';
    }

    // 2. Conflated progress. The timeline outputs frames faster than the
    //    consumer handles them; deleting it when finished ends the stream.
    {
        auto * line = new QTimeLine(160);
        line->setFrameRange(0, 8);
        line->setUpdateInterval(20);
        QObject::connect(line, &QTimeLine::finished, line, &QObject::deleteLater);
        auto frames = QtCoroutine::stream(line, &QTimeLine::frameChanged).latestOnly();
        line->start();
        QtCoroutine::waitFor(watchProgress(std::move(frames)));
    }

    // 3. A timeout on a stream is per-wait, and surviving it is normal.
    {
        QTimer late;
        late.setSingleShot(true);
        auto ticks = QtCoroutine::stream(&late, &QTimer::timeout).withTimeout(100ms);
        late.start(150ms);
        QtCoroutine::waitFor(impatientTick(std::move(ticks)));
    }

    // 4. Cancellation via stop_token, reported as a value.
    {
        QTimer never; // never started
        std::stop_source stop;
        auto ticks = QtCoroutine::stream(&never, &QTimer::timeout).cancelledBy(stop.get_token());
        QTimer::singleShot(30ms, [&stop]() { stop.request_stop(); });
        QtCoroutine::waitFor(waitForTickUntilStopped(std::move(ticks)));
    }

    std::cout << "done\n";
    return 0;
}
