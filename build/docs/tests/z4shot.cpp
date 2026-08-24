#include "mainwindow.h"
#include "overlay.h"
#include <QApplication>
#include <QAction>
#include <QLineEdit>
#include <QKeySequence>
#include <QPixmap>
using namespace mergen;
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    MainWindow w; w.resize(950, 780); w.show();
    w.openPath(QStringLiteral("outline.pdf"));
    for (QAction *a : w.findChildren<QAction *>())
        if (a->shortcut() == QKeySequence(QStringLiteral("Ctrl+K"))) a->trigger();
    Overlay *ov = w.findChild<Overlay *>();
    for (QLineEdit *e : ov->findChildren<QLineEdit *>())
        if (e->accessibleName() == "Command") e->setText(QString::fromUtf8(argc > 1 ? argv[1] : ""));
    app.processEvents();
    w.grab().save(QString::fromUtf8(argc > 2 ? argv[2] : "command.png"));
    return 0;
}
