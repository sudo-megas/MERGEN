#include "mainwindow.h"
#include <QApplication>
#include <QDir>
#include <QPixmap>
#include <QEventLoop>
#include <QTimer>
#include "pageview.h"
static void settle(int ms){QEventLoop l;QTimer::singleShot(ms,&l,&QEventLoop::quit);l.exec();}
using namespace mergen;
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    MainWindow w; w.resize(1100, 700); w.show();
    w.openPath(QStringLiteral("outline.pdf"));
    app.processEvents();
    w.enterCompare(QDir::current().absoluteFilePath(QStringLiteral("outline-v2.pdf")));
    settle(300);
    // Go to the page that actually differs.
    for (PageView *v : w.findChildren<PageView *>()) { v->scrollToPage(1); break; }
    settle(400);
    w.grab().save(QStringLiteral("compare.png"));
    return 0;
}
