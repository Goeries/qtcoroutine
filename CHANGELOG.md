# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0-alpha] - 2026-06-07

### Added

- Awaitable Qt signals via `QtCoroutine::connect()` / `signal()` with a chainable builder
  (`.resumeOn()`, `.cancelledBy()`, `.withTimeout()`, `.readyIf()`, `.asExpected()`).
- `QTask<T>` coroutine return type with `.then()`, `.onCancelled()`, `.onError()`,
  cancellation state, and `QFuture` bridging via `.toFuture()`.
- `QFuture<T>` coroutine traits: functions returning `QFuture<T>` are coroutines, and
  `QFuture<T>` is awaitable with `co_await`.
- Combinators and utilities: `whenAll()`, `tryAll()`, `whenAny()`, `withTimeout()`,
  `cancelledBy()`, and `sleep()`.
- `std::expected`-based error handling via `.asExpected()`.

[Unreleased]: https://github.com/Goeries/qtcoroutine/compare/v0.1.0-alpha...HEAD
[0.1.0-alpha]: https://github.com/Goeries/qtcoroutine/releases/tag/v0.1.0-alpha
