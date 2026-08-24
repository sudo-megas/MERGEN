// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "control.h"

#include <QDir>
#include <QLocalServer>
#include <QPointer>
#include <QTimer>
#include <QLocalSocket>
#include <QStandardPaths>

#include <unistd.h>

namespace mergen {
namespace {

/// Long enough for a socket on the same machine, short enough that a stale file
/// left by a crashed instance does not make the next launch feel broken.
constexpr int kProbeMs = 200;
constexpr int kReplyMs = 1000;

/// How long a connected peer may stay silent before it is dropped. Nothing
/// waits on this — it is a timer, not a blocking read — so a silent client
/// costs the reader nothing at all.
constexpr int kIdleMs = 2000;

/// A command line long enough to be a mistake. Bounded so a peer cannot make
/// the viewer accumulate without limit.
constexpr qint64 kMaxLine = 64 * 1024;

} // namespace

Control::Control(Handler handler, QObject *parent)
    : QObject(parent), m_server(new QLocalServer(this)), m_handler(std::move(handler)) {
    // Only this user. The socket carries commands that open files as them.
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    connect(m_server, &QLocalServer::newConnection, this, &Control::onConnection);
}

QString Control::socketPath() {
    QString directory = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (directory.isEmpty()) {
        directory = QDir::tempPath();
    }
    return QStringLiteral("%1/mergen-%2.sock").arg(directory).arg(getuid());
}

bool Control::listen() {
    const QString path = socketPath();

    // Ask before taking it. If something answers, an instance is already
    // running and this process is the second one.
    QLocalSocket probe;
    probe.connectToServer(path);
    if (probe.waitForConnected(kProbeMs)) {
        probe.disconnectFromServer();
        return false;
    }

    // Nothing answered, so any file still sitting there was left by an instance
    // that died. Inheriting it would mean listening on a socket nobody can
    // reach; removing it is the only way to take over.
    QLocalServer::removeServer(path);
    return m_server->listen(path);
}

QString Control::send(const QString &line) {
    QLocalSocket socket;
    socket.connectToServer(socketPath());
    if (!socket.waitForConnected(kProbeMs)) {
        return QString();
    }
    socket.write(line.toUtf8().trimmed() + '\n');
    if (!socket.waitForBytesWritten(kReplyMs) || !socket.waitForReadyRead(kReplyMs)) {
        return QString();
    }
    return QString::fromUtf8(socket.readAll()).trimmed();
}

void Control::onConnection() {
    while (QLocalSocket *client = m_server->nextPendingConnection()) {
        connect(client, &QLocalSocket::disconnected, client, &QLocalSocket::deleteLater);
        connect(client, &QLocalSocket::readyRead, this, [this, client] { onReadyRead(client); });

        // A peer that connects and says nothing is dropped by a timer rather
        // than waited on. Waiting here blocked the whole interface: one silent
        // client cost about a second of frozen UI, and several cost several.
        auto *idle = new QTimer(client);
        idle->setSingleShot(true);
        connect(idle, &QTimer::timeout, client, [client] { client->disconnectFromServer(); });
        idle->start(kIdleMs);
    }
}

void Control::onReadyRead(QLocalSocket *client) {
    // One line per connection, so a reply cannot be mistaken for the answer to
    // a later command.
    if (!client->canReadLine()) {
        if (client->bytesAvailable() > kMaxLine) {
            client->write("err: line too long\n");
            client->flush();
            client->disconnectFromServer();
        }
        return;
    }

    const QString line = QString::fromUtf8(client->readLine(kMaxLine)).trimmed();
    const int space = line.indexOf(QLatin1Char(' '));
    const QString verb = (space < 0 ? line : line.left(space)).toLower();
    const QString argument = space < 0 ? QString() : line.mid(space + 1).trimmed();

    // A command already in flight may be sitting in a nested event loop with a
    // dialog up. Answering plainly beats re-entering the handler underneath it.
    if (m_busy) {
        client->write("err: busy\n");
        client->flush();
        client->disconnectFromServer();
        return;
    }

    // The peer can hang up while the handler is inside a nested loop, and the
    // socket's own deleteLater is then collected by that loop rather than
    // deferred past it. Hold a guard and check it before replying.
    QPointer<QLocalSocket> alive(client);
    m_busy = true;
    const QString reply = m_handler ? m_handler(verb, argument) : QStringLiteral("err: not ready");
    m_busy = false;

    if (!alive) {
        return;
    }
    alive->write(reply.toUtf8() + '\n');
    alive->flush();
    alive->disconnectFromServer();
}

} // namespace mergen
