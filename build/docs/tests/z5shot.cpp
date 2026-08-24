#include "mainwindow.h"
#include "pageview.h"
#include <QApplication>
#include <QPixmap>
using namespace mergen;
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    MainWindow w; w.resize(560, 620); w.show();
    w.openPath(QString::fromUtf8(argv[1]));
    PageView *v = w.findChild<PageView *>();
    if (argc > 3 && QString::fromUtf8(argv[3]) == "night") v->setNightMode(true);
    app.processEvents();
    w.grab().save(QString::fromUtf8(argv[2]));
    return 0;
}
