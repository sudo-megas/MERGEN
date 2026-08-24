// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QObject>
#include <QString>

#include <functional>

class QLocalServer;
class QLocalSocket;

namespace mergen {

/// The control surface: a Unix socket that lets a keybinding drive MERGEN.
///
/// Readers on a tiling compositor script everything else they own, and a viewer
/// they cannot drive from a key is a viewer that does not belong on that desk.
/// Every command here is one they could already have typed; the socket adds
/// reach, not capability — MZ.md §9.
///
/// A socket rather than D-Bus, deliberately. Both come free inside `qt6-base`,
/// so the choice is not about dependencies but about what has to be running: a
/// session bus is desktop-environment infrastructure and this project has
/// refused to require any since v1.0. A socket in `$XDG_RUNTIME_DIR` needs
/// nothing beyond the kernel and is drivable from a shell one-liner. It also
/// has no network surface by construction, which is what keeps it on the right
/// side of §5's ban.
class Control : public QObject {
    Q_OBJECT

public:
    /// Answers one command. Returns the text to send back, which is `ok` or a
    /// sentence beginning `err: `.
    using Handler = std::function<QString(const QString &verb, const QString &argument)>;

    explicit Control(Handler handler, QObject *parent = nullptr);

    /// Takes the socket, and with it the role of the running instance. Returns
    /// false when another instance already holds it — the caller is then the
    /// second one and should hand over whatever it was asked to open.
    bool listen();

    /// Sends one line to a running instance and waits briefly for its reply.
    /// Returns an empty string when nothing is listening, which is the ordinary
    /// case of being the only instance.
    static QString send(const QString &line);

    /// `$XDG_RUNTIME_DIR/mergen-$UID.sock`, falling back to the temp directory
    /// on a system that sets no runtime directory.
    static QString socketPath();

private:
    void onConnection();
    void onReadyRead(QLocalSocket *client);

    QLocalServer *m_server = nullptr;
    Handler m_handler;

    /// A command can enter a nested event loop (the password prompt, the
    /// pkexec dialog), during which this server keeps accepting. Without this
    /// a second command re-enters the handler while the first is still on the
    /// stack — MZ.md §9.
    bool m_busy = false;
};

} // namespace mergen
