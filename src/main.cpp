// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "control.h"
#include "mainwindow.h"

#include <QApplication>
#include <QFileInfo>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    QApplication::setApplicationName(QStringLiteral("mergen"));
    QApplication::setApplicationDisplayName(QStringLiteral("MERGEN"));
    QApplication::setApplicationVersion(QStringLiteral(MERGEN_VERSION));
    QApplication::setOrganizationName(QStringLiteral("MEGAS"));
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
