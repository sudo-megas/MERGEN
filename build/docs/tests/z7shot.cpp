#include "mainwindow.h"
#include "pageview.h"
#include <QApplication>
#include <QPixmap>
using namespace mergen;
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    MainWindow w; w.resize(760, 620); w.show();
    if (argc > 2) w.openPath(QString::fromUtf8(argv[2]));
    app.processEvents();
    w.grab().save(QString::fromUtf8(argv[1]));
    return 0;
}
