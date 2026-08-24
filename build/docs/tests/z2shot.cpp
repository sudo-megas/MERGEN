#include "mainwindow.h"
#include "overlay.h"
#include <QApplication>
#include <QAction>
#include <QKeySequence>
#include <QPixmap>
#include <cstdio>
using namespace mergen;
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    MainWindow w;
    w.resize(900, 700);
    w.show();
    w.openPath(QString::fromUtf8(argc > 1 ? argv[1] : "test.pdf"));
    for (QAction *a : w.findChildren<QAction *>())
        if (a->shortcut() == QKeySequence(QStringLiteral("Ctrl+I"))) a->trigger();
    app.processEvents();
    w.grab().save(QString::fromUtf8(argc > 2 ? argv[2] : "overlay.png"));
    std::printf("saved\n");
    return 0;
}
