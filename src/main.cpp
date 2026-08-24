// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "control.h"
#include "iconset.h"
#include "mainwindow.h"

#include <QApplication>
#include <QIcon>
#include <QFileInfo>

#include <csignal>

int main(int argc, char *argv[]) {
    // Two signals that would otherwise end the process where a failed write
    // should have been reported instead. SIGXFSZ fires when a file-size limit
    // is hit — and recent.toml is written on every successful open, so that is
    // the most travelled path in the application. Ignoring them turns both into
    // an errno the caller can see.
    ::signal(SIGXFSZ, SIG_IGN);
    ::signal(SIGPIPE, SIG_IGN);

    QApplication app(argc, argv);

    QApplication::setApplicationName(QStringLiteral("mergen"));
    // Deliberately no setApplicationDisplayName. Qt appends " — <display name>"
    // to every window title at the platform layer, and this application's titles
    // already open with MERGEN — the result read
    // "MERGEN ///— outline.pdf —\\\ MEGAS — MERGEN".
    QApplication::setApplicationVersion(QStringLiteral(MERGEN_VERSION));
    QApplication::setOrganizationName(QStringLiteral("MEGAS"));

    // The installed icon theme first, so a system that has themed the icon gets
    // its version; the embedded copy otherwise, which is what makes the icon
    // appear when running from the build directory rather than from a package.
    QIcon icon = QIcon::fromTheme(QStringLiteral("mergen"));
    if (icon.isNull()) {
        icon = mergen::applicationIcon();
    }
    QApplication::setWindowIcon(icon);
    // Lets the compositor map the Wayland app_id onto mergen.desktop, and with
    // it the installed hicolor icon.
    QGuiApplication::setDesktopFileName(QStringLiteral("mergen"));

    const QString path =
        argc > 1 ? QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath() : QString();

    mergen::MainWindow window;

    // Taking the socket is also how a second launch is noticed. If another
    // instance holds it, hand over the file and leave rather than opening a
    // second window onto the same desk.
    if (!window.listenForCommands()) {
        if (!path.isEmpty()) {
            mergen::Control::send(QStringLiteral("open ") + path);
        }
        return 0;
    }

    // No default size, no minimum size, no saved geometry: Niri places it.
    if (!path.isEmpty()) {
        window.openPath(path);
    }

    window.show();
    return app.exec();
}
