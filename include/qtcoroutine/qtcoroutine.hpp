#pragma once
#include <coroutine>
#include <QFuture>
#include <QFutureWatcher>

namespace QtCoroutine {

template<typename T>
class QTask {
public:
    struct promise_type {
        QTask<T> get_return_object() noexcept {
            return QTask<T>::fromFuture(std::move(qpromise.future()));
        }

        std::suspend_never initial_suspend() noexcept {
            qpromise.start();
            return {};
        }

        std::suspend_never final_suspend() noexcept {
            return {};
        }

        void return_value(const T & value) noexcept {
            qpromise.addResult(value);
            qpromise.finish();
        }

        void unhandled_exception() noexcept {
            qpromise.setException(std::current_exception());
            qpromise.finish();
        }

        QPromise<T> qpromise;
    };

    static QTask<T> fromFuture(QFuture<T> && future) {
        return QTask<T>(std::move(future));
    }

    bool await_ready() const noexcept {
        return m_future.isFinished();
    }

    void await_suspend(std::coroutine_handle<> handle) {
        // m_handle = handle;  // Reserved for future use (e.g. cancellation support)

        // Lambda will only execute if watcher still exists (thus tied to lifetime of QTask instance)
        m_connection = QObject::connect(&m_watcher, &QFutureWatcher<T>::finished,
                                        &m_watcher, [handle]() {
                                            handle.resume();
                                        });

        m_watcher.setFuture(m_future);
    }

    [[nodiscard]] T result() {
        m_future.waitForFinished();
        if (m_future.isValid())
            return m_future.result();
        else
            throw std::runtime_error("QTask cannot get result of invalid QFuture");
    }

    void waitForFinished() {
        m_future.waitForFinished();
    }

    bool isFinished() const noexcept {
        return m_future.isFinished();
    }

    // Qt wraps exceptions in QUnhandledException when propagating across threads via QFuture.
    // We unwrap and rethrow the original exception so callers get natural exception propagation.
    [[nodiscard]] T await_resume() {
        try {
            m_future.waitForFinished();
        } catch (QUnhandledException & e) {
            if (e.exception())
                std::rethrow_exception(e.exception());
        }

        if (m_future.isValid())
            return m_future.takeResult();
        else
            throw std::runtime_error("QTask cannot await_resume invalid QFuture");

    }

private:
    QTask(QFuture<T> && future)
        : m_future(future)
    {}

    // std::coroutine_handle<> m_handle;  // Reserved for future use
    QFuture<T> m_future;
    QFutureWatcher<T> m_watcher;
    QMetaObject::Connection m_connection;
};

// Void specialization
template<>
class QTask<void> {
public:
    struct promise_type {
        QTask<void> get_return_object() noexcept {
            return QTask<void>::fromFuture(std::move(qpromise.future()));
        }

        std::suspend_never initial_suspend() noexcept {
            qpromise.start();
            return {};
        }

        std::suspend_never final_suspend() noexcept {
            return {};
        }

        void return_void() noexcept {
            qpromise.finish();
        }

        void unhandled_exception() noexcept {
            qpromise.setException(std::current_exception());
            qpromise.finish();
        }

        QPromise<void> qpromise;
    };

    static QTask<void> fromFuture(QFuture<void> && future) {
        return QTask<void>(std::move(future));
    }

    bool await_ready() const noexcept {
        return m_future.isFinished();
    }

    void await_suspend(std::coroutine_handle<> handle) {
        // m_handle = handle;  // Reserved for future use (e.g. cancellation support)

        // Lambda will only execute if watcher still exists (thus tied to lifetime of QTask instance)
        m_connection = QObject::connect(&m_watcher, &QFutureWatcher<void>::finished,
                                        &m_watcher, [handle]() {
                                            handle.resume();
                                        });

        m_watcher.setFuture(m_future);
    }

    void waitForFinished() {
        m_future.waitForFinished();
    }

    bool isFinished() const noexcept {
        return m_future.isFinished();
    }

    // Qt wraps exceptions in QUnhandledException when propagating across threads via QFuture.
    // We unwrap and rethrow the original exception so callers get natural exception propagation.
    void await_resume() {
        try {
            m_future.waitForFinished();
        } catch (QUnhandledException & e) {
            if (e.exception())
                std::rethrow_exception(e.exception());
        }
    }

private:
    QTask(QFuture<void> && future)
        : m_future(future)
    {}

    // std::coroutine_handle<> m_handle;  // Reserved for future use
    QFuture<void> m_future;
    QFutureWatcher<void> m_watcher;
    QMetaObject::Connection m_connection;
};

template<typename Sender, typename Signal, typename = QtPrivate::EnableIfInvocable<Sender, Signal>>
QTask<QtFuture::ArgsType<Signal>> connect(Sender *sender, Signal signal) {
    auto future = QtFuture::connect(sender, signal);
    return QTask<QtFuture::ArgsType<Signal>>::fromFuture(std::move(future));
}

}  // namespace QtCoroutine
