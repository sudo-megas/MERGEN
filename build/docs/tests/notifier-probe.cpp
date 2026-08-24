#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QEventLoop>
#include <QTimer>
#include <QDir>
#include <cstdio>
int fails = 0;
static void check(bool ok, const char *what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString path = QDir::tempPath() + QStringLiteral("/mergen-notifier-probe.sock");
    QLocalServer::removeServer(path);
    QLocalServer server;
    if (!server.listen(path)) { std::printf("  cannot listen\n"); return 2; }

    bool served = false;
    QObject::connect(&server, &QLocalServer::newConnection, [&] {
        QLocalSocket *c = server.nextPendingConnection();
        QObject::connect(c, &QLocalSocket::readyRead, [&served, c] {
            served = true; c->readAll();
        });
    });

    QLocalSocket client;
    client.connectToServer(path);
    if (!client.waitForConnected(2000)) { std::printf("  cannot connect\n"); return 2; }
    // Let the server accept the connection.
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();

    client.write("open /etc/passwd\n");
    client.flush();
    client.waitForBytesWritten(1000);

    // This is exactly what the print loop does, once per page.
    for (int i = 0; i < 40; ++i)
        QCoreApplication::processEvents(QEventLoop::ExcludeSocketNotifiers);
    check(!served, "a socket command does NOT arrive under ExcludeSocketNotifiers");

    // And once the loop ends, it is delivered rather than lost.
    for (int i = 0; i < 40; ++i) QCoreApplication::processEvents();
    check(served, "and IS delivered once the loop ends - nothing is dropped");

    QLocalServer::removeServer(path);
    std::printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", fails);
    return fails;
}
