// #include <QCoreApplication>
// #include <QNetworkAccessManager>
// #include <QNetworkReply>
// #include "include/qtcoroutine/qtcoroutine.hpp"
// #include "include/qtcoroutine/qfuture_coroutine_traits.hpp"
// #include "include/qtcoroutine/qtask.hpp"

// using namespace QtCoroutine;

// class Client : public QObject {
//     Q_OBJECT

//     QTask<int> awe();

// signals:
//     void userCreated(int id, QString name);
// };

// QFuture<int> someCoroutine(std::stop_token st = {}) {
//     QNetworkAccessManager nam;
//     auto * reply = nam.get({});

//     co_await QtCoroutine::connect(reply, &QNetworkReply::finished);

//     MyObj obj;
//     auto [id, name] = co_await QtCoroutine::connect(&obj, &MyObj::userCreated);

//     qDebug() << reply->readAll();
//     reply->deleteLater();

//     co_return 11;
// }


// QtCoroutine::QTask<int> taskDemo() {
//     // Typical use case - chained network I/O


//     // Local variable - will survive suspend and clean up when coroutine handle cleans up
//     QNetworkAccessManager nam;

//     QNetworkRequest request(QUrl{"https://www.google.com"});
//     auto * reply = nam.get(request);


//     co_await QtCoroutine::connect(reply, &QNetworkReply::finished);

//     MyObj obj;
//     auto [id, name] = co_await QtCoroutine::connect(&obj, &MyObj::userCreated);

//     qDebug() << reply->readAll();
//     reply->deleteLater();

//     co_return 33;
// }

// int main(int argc, char *argv[])
// {
//     QCoreApplication a(argc, argv);

//     // Set up code that uses the Qt event loop here.
//     // Call a.quit() or a.exit() to quit the application.
//     // A not very useful example would be including
//     // #include <QTimer>
//     // near the top of the file and calling
//     // QTimer::singleShot(5000, &a, &QCoreApplication::quit);
//     // which quits the application after 5 seconds.

//     // If you do not need a running Qt event loop, remove the call
//     // to a.exec() or use the Non-Qt Plain C++ Application template.

//     return a.exec();
// }
