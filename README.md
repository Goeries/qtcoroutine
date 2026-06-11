# QtCoroutine

[![CI](https://github.com/Goeries/qtcoroutine/actions/workflows/ci.yml/badge.svg)](https://github.com/Goeries/qtcoroutine/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)
![Qt6](https://img.shields.io/badge/Qt-6-41cd52.svg)

A C++23 coroutine library for Qt 6. Header-only, designed to make asynchronous Qt code read like sequential code.

```cpp
#include <QtCoroutine>

QtCoroutine::QTask<QByteArray> fetchData(QNetworkAccessManager & nam, QUrl url, std::stop_token st) {
    auto * reply = nam.get(QNetworkRequest{url});

    co_await QtCoroutine::connect(reply, &QNetworkReply::finished)
        .cancelledBy(st)
        .withTimeout(std::chrono::seconds(30));

    auto data = reply->readAll();
    reply->deleteLater();
    co_return data;
}
```

## Features

### Awaitable Qt Signals

`co_await` any Qt signal with a chainable builder API:

```cpp
// Void signal
co_await QtCoroutine::connect(&button, &QPushButton::clicked);

// Signal with arguments — automatically unwrapped
auto text = co_await QtCoroutine::connect(&edit, &QLineEdit::textChanged);

// Structured bindings for multi-arg signals
auto [id, name] = co_await QtCoroutine::connect(&api, &Api::userCreated);

// Full builder chain
auto result = co_await QtCoroutine::connect(&obj, &Obj::finished)
    .resumeOn(&context)                    // resume on a specific thread
    .cancelledBy(stopToken)                // cancellation via std::stop_token
    .withTimeout(std::chrono::seconds(5))  // timeout support
    .readyIf([](Obj * o) -> std::optional<int> {  // skip if already ready
        if (o->hasResult()) return o->result();
        return std::nullopt;
    });
```

### Signal Streams

`co_await connect(...)` captures one emission. For signals that fire repeatedly, a stream connects once — at construction, so nothing is missed — queues every emission, and hands them out one per `co_await`:

```cpp
auto bytes = QtCoroutine::stream(&socket, &QIODevice::readyRead);
while (co_await bytes.next()) {              // void signal → bool
    consume(socket.readAll());
}

auto progress = QtCoroutine::stream(&worker, &Worker::progress)
    .latestOnly();                           // conflate: only the newest value is kept
while (auto p = co_await progress.next()) {  // int signal → std::optional<int>
    updateBar(*p);
}
```

The sender being destroyed ends the stream like reaching EOF: queued values still drain, then `next()` returns `false`/`nullopt` and the loop exits — no exception, no try/catch for ordinary object teardown. Caller-side aborts do throw, consistent with one-shot awaits: `.cancelledBy(st)` → `AwaitCancelled{Stopped}` (immediate, terminal), `.withTimeout(ms)` → `AwaitCancelled{Timeout}` for any wait that goes that long without an emission (non-fatal — the stream stays armed and usable). `next().asExpected()` opts into `std::expected` instead. Buffering is unbounded by default; multi-arg signals yield `std::optional<std::tuple<...>>`, and the stream type is spelled by the signal's argument types — `QSignalStream<int>` — so it stores cleanly as a member.

### QTask — Coroutine Return Type

`QTask<T>` is a coroutine type with continuations, cancellation state, and QFuture bridging:

```cpp
QtCoroutine::QTask<int> compute() {
    co_await QtCoroutine::sleep(std::chrono::seconds(1));
    co_return 42;
}

// From non-coroutine code
auto task = compute();
task.then([](int val) { qDebug() << val; });
task.onCancelled([](const auto & err) { qWarning() << err.message(); });
task.onError([](std::exception_ptr ep) { /* handle */ });

// Bridge to QFuture for QFutureWatcher, QtFuture::whenAll, etc.
QFuture<int> future = task.toFuture();
```

**Lifetime.** A `QTask` owns its coroutine frame; destroying the task destroys the frame, cancelling any pending awaits. Either `co_await` it from another coroutine, keep it alive until it settles, or call `.detach()` for fire-and-forget — the frame then self-destructs on completion. `QTask` is `[[nodiscard]]`, so accidentally discarding one (`compute();`) is a compiler warning rather than silently lost work. Callbacks fire while the coroutine is suspended, which makes destroying or reassigning a task from inside its own `.then()` callback safe — frame destruction is deferred, like `QObject::deleteLater()` called from a slot.

### QFuture as Coroutine Return Type

Functions returning `QFuture<T>` are automatically coroutines:

```cpp
#include <QtCoroutine/QFutureCoroutineTraits>

QFuture<int> pipeline() {
    auto raw = co_await QtConcurrent::run([] { return fetchRawData(); });
    auto processed = co_await QtConcurrent::run([&] { return process(raw); });
    co_return processed;
}
```

Exceptions thrown by the awaited future rethrow at the `co_await` (unwrapped from `QUnhandledException`), and cancelling the awaited future resumes the coroutine with `AwaitCancelled` once the future settles.

### Value-Based Error Handling

Opt into `std::expected` instead of exceptions with `.asExpected()`:

```cpp
auto result = co_await QtCoroutine::connect(&obj, &Obj::finished)
    .cancelledBy(st)
    .asExpected();

if (!result) {
    qWarning() << result.error().message();  // "Stop request received"
    co_return;
}
auto value = *result;
```

`QTask` also works directly with `std::expected` as a return type:

```cpp
QTask<std::expected<void, QString>> tryConnect(Client & c, std::stop_token st) {
    auto reply = co_await QtCoroutine::connect(&c, &Client::connected)
        .cancelledBy(st)
        .asExpected();

    if (!reply)
        co_return std::unexpected{reply.error().message()};

    co_return {};  // success — use {} instead of bare co_return
}
```

### Combinators and Utilities

```cpp
// Wait for all tasks (waits for every task to settle before propagating errors)
auto [r1, r2, r3] = co_await QtCoroutine::whenAll(task1, task2, task3);

// Wait for all tasks (short-circuits on first error)
auto [r1, r2] = co_await QtCoroutine::tryAll(task1, task2);

// Race tasks — the first to settle wins
auto [index, value] = co_await QtCoroutine::whenAny(taskA, taskB);  // same T: winner index + value
std::size_t winner = co_await QtCoroutine::whenAny(voidA, voidB);   // all void: winner index
auto either = co_await QtCoroutine::whenAny(intTask, stringTask);   // mixed: std::variant<int, QString>

// Timeout on any QTask
auto result = co_await QtCoroutine::withTimeout(task, std::chrono::seconds(5));

// Cancellation on any QTask
auto result = co_await QtCoroutine::cancelledBy(task, stopToken);

// Sleep
co_await QtCoroutine::sleep(std::chrono::milliseconds(500));
```

### Blocking Bridge — `waitFor`

For `main()`, tests, and other non-coroutine code, `waitFor` blocks on a nested event loop until the task settles, then returns its value or rethrows its cancellation/error:

```cpp
int value = QtCoroutine::waitFor(compute());
```

## Headers

| Header | Purpose |
|--------|---------|
| `QtCoroutine/qtcoroutine.hpp` | Signal awaiting, builder pattern, `sleep()` |
| `QtCoroutine/qsignalstream.hpp` | `QSignalStream` — lossless `co_await` loops over repeating signals |
| `QtCoroutine/qtask.hpp` | `QTask<T>`, `whenAll`, `tryAll`, `whenAny`, `withTimeout`, `cancelledBy` |
| `QtCoroutine/qfuture_coroutine_traits.hpp` | `QFuture<T>` as coroutine return type + `co_await` |
| `QtCoroutine/utils.hpp` | Type utilities, `AwaitCancelled` |

## Cancellation

The library uses `std::stop_token` for cancellation, which propagates automatically through coroutine chains via `AwaitCancelled` exceptions. QTask catches these and stores them as state:

```cpp
QtCoroutine::QTask<void> work(std::stop_token st) {
    // AwaitCancelled propagates automatically if st is cancelled
    co_await QtCoroutine::connect(&obj, &Obj::step1Done).cancelledBy(st);
    co_await QtCoroutine::connect(&obj, &Obj::step2Done).cancelledBy(st);
}

auto task = work(stopSource.get_token());
stopSource.request_stop();  // cancels wherever the coroutine is suspended

// From non-coroutine code
if (task.isCancelled())
    qDebug() << *task.cancelReason();  // Stopped, SenderDestroyed, Timeout, ...
```

`cancelReason()` returns `std::optional<AwaitCancelled::Reason>`, engaged only when the task was actually cancelled (it still compares directly against reasons: `task.cancelReason() == AwaitCancelled::Stopped`). `QtCoroutine::AwaitCancelled` is the public spelling of the cancellation type.

## Threading

The defaults follow `QObject::connect`: the suspended coroutine is the receiver, and resuming it is the slot.

- A `co_await` resumes on the thread it suspended on, regardless of which thread the sender lives on or emits from — code after the `co_await` runs on the same thread as the code before it. A same-thread emission resumes synchronously inside the `emit`, exactly like a direct-connected slot.
- `.resumeOn(ctx)` explicitly migrates the coroutine: delivery and everything after the `co_await` run on `ctx`'s thread.
- A signal stream *pins* its consumer instead of migrating it: `next()` is awaited on the stream's construction thread — or, with `.resumeOn(ctx)`, on `ctx`'s thread from the first `next()` on (debug-asserted). Emissions remain unrestricted inputs from any thread; values are queued from the emitting thread and the wake is marshalled to the consumer.
- Inputs are unrestricted: signals may be emitted, futures completed, and `request_stop()` called from any thread; the library marshals the resume back to the right thread.
- The task handle is thread-affine: while a task is live, owner-side operations — `co_await`, `then`/`onCancelled`/`onError`, `toFuture`, `detach`, destruction — belong on the thread that created it (debug-asserted). Once settled, querying and destroying are safe from any thread that has synchronized with the completion. After migrating with `resumeOn`, bridge results back with `toFuture()`.

## Integration

### Git submodule (recommended)

Add the library under `external/` so its version is pinned in your repository:

```bash
git submodule add https://github.com/Goeries/qtcoroutine.git external/qtcoroutine
git -C external/qtcoroutine checkout v0.1.0-alpha   # optional: pin to a release
```

Then add it from your `CMakeLists.txt`:

```cmake
add_subdirectory(external/qtcoroutine)
target_link_libraries(myapp PRIVATE qtcoroutine::qtcoroutine)
```

Anyone cloning your project pulls the library along with it:

```bash
git clone --recurse-submodules <your-repo>
# or, in an existing clone:
git submodule update --init --recursive
```

### FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(qtcoroutine
    GIT_REPOSITORY https://github.com/Goeries/qtcoroutine.git
    GIT_TAG v0.1.0-alpha
)
FetchContent_MakeAvailable(qtcoroutine)

target_link_libraries(myapp PRIVATE qtcoroutine::qtcoroutine)
```

This automatically sets up include paths, C++23, and Qt6 dependencies.

### Subdirectory

```cmake
add_subdirectory(qtcoroutine)
target_link_libraries(myapp PRIVATE qtcoroutine::qtcoroutine)
```

### Copy headers

Copy `include/` into your project and add **both** directories to your include path — the first for `#include <QtCoroutine/qtask.hpp>` style includes, the second so the umbrella `#include <QtCoroutine>` resolves:

```cmake
target_include_directories(myapp PRIVATE path/to/include path/to/include/QtCoroutine)
target_link_libraries(myapp PRIVATE Qt6::Core Qt6::Concurrent)
```

## Requirements

- C++23 (GCC 13+, MSVC 2022 17.5+; Clang with libstdc++ is currently unsupported — libstdc++'s `std::expected` requires GCC)
- Qt 6
- CMake 3.22+
- A running Qt event loop on every awaiting thread (asserted in debug builds; in a release build an await without one simply never resumes)

## Naming: `connect()` vs `signal()`

`QtCoroutine::connect()` and `QtCoroutine::signal()` are the exact same function. The examples use `connect()` because it mirrors `QObject::connect` and `QtFuture::connect`. If that name ever clashes in your code — for example with `QObject::connect` or `QtFuture::connect` brought in unqualified via `using namespace` — use `signal()` instead; it's identical, just collision-free.

## Tested On

| Compiler | Platform |
|----------|----------|
| GCC 13.3 | Ubuntu 24.04 — including AddressSanitizer and ThreadSanitizer CI jobs |
| MSVC 2022 | Windows |
| MinGW GCC 13 | Windows 11 |

## License

[MIT](LICENSE) © 2026 Jeandré Gouws
