// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "control.h"

#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>

#include <unistd.h>

namespace mergen {
namespace {

/// Long enough for a socket on the same machine, short enough that a stale file
/// left by a crashed instance does not make the next launch feel broken.
constexpr int kProbeMs = 200;
constexpr int kReplyMs = 1000;

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

        // One line per connection, so a reply cannot be mistaken for the answer
        // to a later command.
        if (!client->waitForReadyRead(kReplyMs)) {
            client->disconnectFromServer();
            continue;
        }

        const QString line = QString::fromUtf8(client->readLine()).trimmed();
        const int space = line.indexOf(QLatin1Char(' '));
        const QString verb = (space < 0 ? line : line.left(space)).toLower();
        const QString argument = space < 0 ? QString() : line.mid(space + 1).trimmed();

        const QString reply =
            m_handler ? m_handler(verb, argument) : QStringLiteral("err: not ready");
        client->write(reply.toUtf8() + '\n');
        client->flush();
        client->disconnectFromServer();
    }
}

} // namespace mergen
