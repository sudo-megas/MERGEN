// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    QApplication::setApplicationName(QStringLiteral("mergen"));
    QApplication::setApplicationDisplayName(QStringLiteral("MERGEN"));
    QApplication::setApplicationVersion(QStringLiteral(MERGEN_VERSION));
    QApplication::setOrganizationName(QStringLiteral("MEGAS"));
    // Lets the compositor map the Wayland app_id onto mergen.desktop, and with
    // it the installed hicolor icon.
    QGuiApplication::setDesktopFileName(QStringLiteral("mergen"));

    mergen::MainWindow window;

    // No default size, no minimum size, no saved geometry: Niri places it.
    if (argc > 1) {
        window.openPath(QString::fromLocal8Bit(argv[1]));
    }

    window.show();
    return app.exec();
}
