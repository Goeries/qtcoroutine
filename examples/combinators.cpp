// A tour of QtCoroutine's task combinators: racing with whenAny, timeouts,
// and stop_token cancellation — all consumed from plain main() through
// waitFor(), with no app.exec() scaffolding (waitFor pumps the event loop).
//
// It is built (and run) in CI so these snippets can't silently rot.

#include <QCoreApplication>
#include <QTimer>

#include <chrono>
#include <iostream>
#include <stop_token>

#include <QtCoroutine>

using namespace std::chrono_literals;

// A pretend fetch: completes with `value` after `delay`.
QtCoroutine::QTask<int> fetchAfter(std::chrono::milliseconds delay, int value) {
    co_await QtCoroutine::sleep(delay);
    co_return value;
}

QtCoroutine::QTask<QtCoroutine::WhenAnyResult<int>> raceFetches(QtCoroutine::QTask<int> & a,
                                                                QtCoroutine::QTask<int> & b) {
    co_return co_await QtCoroutine::whenAny(a, b);
}

// Waits on a signal that never fires — only the stop token can end it.
QtCoroutine::QTask<void> waitUntilCancelled(std::stop_token st) {
    QTimer never; // single-shot, never started
    co_await QtCoroutine::signal(&never, &QTimer::timeout).cancelledBy(st);
}

int main(int argc, char ** argv) {
    QCoreApplication app(argc, argv);

    // 1. Race two sources — the first to settle wins, and whenAny reports
    //    which one it was.
    {
        auto primary = fetchAfter(50ms, 1);
        auto fallback = fetchAfter(10ms, 2);
        auto [index, value] = QtCoroutine::waitFor(raceFetches(primary, fallback));
        std::cout << "race: task " << index << " won with value " << value << '\n'; // task 1, value 2
    }

    // 2. Timeout: wrap any task; expiry surfaces as AwaitCancelled{Timeout}.
    //    The slow inner task keeps running (fire-and-forget) until its owner
    //    destroys it at scope exit.
    {
        auto slow = fetchAfter(5s, -1);
        auto guarded = QtCoroutine::withTimeout(slow, 100ms);
        try {
            QtCoroutine::waitFor(guarded);
        } catch (const QtCoroutine::AwaitCancelled & c) {
            std::cout << "timeout: " << c.message() << '\n';
        }
    }

    // 3. Cooperative cancellation with std::stop_token.
    {
        std::stop_source source;
        auto waiting = waitUntilCancelled(source.get_token());
        QTimer::singleShot(50ms, &app, [&]() { source.request_stop(); });
        try {
            QtCoroutine::waitFor(waiting);
        } catch (const QtCoroutine::AwaitCancelled & c) {
            std::cout << "cancelled: " << c.message() << '\n';
        }
    }

    std::cout << "done\n";
    return 0;
}
