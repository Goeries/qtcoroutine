# qtcoroutine

qtcoroutine brings modern C++ coroutine facilities to Qt's event loop system as an immediate alternative to Qt's [signal/slot callback-](https://doc.qt.io/qt-6/signalsandslots.html) and [future-based asynchronous patterns](https://doc.qt.io/qt-6/qfuture.html).
It's lightweight, uses [QPromise](https://doc.qt.io/qt-6/qpromise.html) under the hood, and is designed to be **header-only** for [easy integration with existing projects](#integration).

Until the [Qt Framework](https://www.qt.io/development/qt-framework) supports [coroutines](https://en.cppreference.com/w/cpp/language/coroutines.html) natively, one (or both) of the following headers can easily be added to a project to integrate the use of coroutines with Qt's event loop:
1. `qtcoroutine/qfuture_coroutine.hpp` — Makes `QFuture` **awaitable** (using `co_await`) and usable as a **coroutine return type** (using `co_return`)
2. `qtcoroutine/qtcoroutine.hpp` — Provides a `QTask` alternative if you prefer not to change the behavior of `QFuture` 

Coroutines hold several improvements over callback- and future-based async patterns, including intuitive control flow, natural exception propagation to callers, and a deep level of control for intricate async operations.

## Usage Examples

### `qtcoroutine/qfuture_coroutine.hpp`

```cpp
#include <qtcoroutine/qfuture_coroutine.hpp>

QFuture<int> computeAsync() {
    int result = co_await QtConcurrent::run([] { return 42; });
    co_return result * 2;
}

QFuture<void> doWorkAsync() {
    co_await QtConcurrent::run([] { doWork(); });
    co_return;
}

// Await signals
QFuture<Result> handleReply(QNetworkReply * reply) {
    co_await QtFuture::connect(reply, &QNetworkReply::finished);
    auto result = processReply(reply);
    co_return result;
}
```

### `qtcoroutine/qtcoroutine.hpp`

```cpp
#include <qtcoroutine/qtcoroutine.hpp>
using namespace QtCoroutine;

QTask<int> computeAsync() {
    auto task = QTask<int>::fromFuture(QtConcurrent::run([] { return 42; }));
    int result = co_await task;
    co_return result * 2;
}

QTask<void> doWorkAsync() {
    auto task = QTask<void>::fromFuture(QtConcurrent::run([] { doWork(); }));
    co_await task;
    co_return;
}

// Await signals
QTask<Result> handleReply(QNetworkReply *reply) {
    co_await QtCoroutine::connect(reply, &QNetworkReply::finished);
    auto result = processReply(reply);
    co_return result;
}

// Blocking access (similar to QFuture)
auto task = computeAsync();
int value = task.result();  // blocks until ready
```

Both headers use `QFutureWatcher` to suspend and resume coroutines through the Qt event loop, ensuring thread-safe signal/slot integration.

## Integration

### FetchContent (recommended)

```cmake
include(FetchContent)
FetchContent_Declare(qtcoroutine
    GIT_REPOSITORY https://github.com/goeries/qtcoroutine.git
    GIT_TAG v1.0.0
)
FetchContent_MakeAvailable(qtcoroutine)

target_link_libraries(myapp PRIVATE qtcoroutine::qtcoroutine)
```

### Subdirectory

Clone or add as a git submodule, then:

```cmake
add_subdirectory(qtcoroutine)
target_link_libraries(myapp PRIVATE qtcoroutine::qtcoroutine)
```

### Copy headers

Copy the `include/qtcoroutine/` directory into your project and add the parent directory to your include path.

## Requirements

- C++20 or later (coroutine support)
- Qt 6

## Tested On

| Compiler | Platform |
|----------|----------|
| GCC 13.3 | Ubuntu 24.04 (WSL2) |
| MinGW GCC | Windows 11 |
