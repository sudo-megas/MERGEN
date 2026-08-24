#include "mainwindow.h"
#include "pageview.h"
#include "overlay.h"
#include "document.h"
#include <QApplication>
#include <QAction>
#include <QKeySequence>
#include <QPixmap>
#include <cstdio>
using namespace mergen;
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    MainWindow w; w.resize(950, 780); w.show();
    w.openPath(QStringLiteral("outline.pdf"));
    app.processEvents();
    PageView *v = w.findChild<PageView *>();

    // Hit-test the link by sweeping the viewport, which needs no internals.
    int hitX = -1, hitY = -1, target = -1;
    for (int y = 0; y < v->viewport()->height() && hitY < 0; y += 3)
        for (int x = 0; x < v->viewport()->width(); x += 3)
            if (const PageLink *lk = v->linkAt(QPoint(x, y))) {
                hitX = x; hitY = y; target = lk->page; goto done;
            }
done:
    std::printf("link hit-test: %s at (%d,%d) -> page %d\n",
                hitX >= 0 ? "FOUND" : "NOT FOUND", hitX, hitY, target + 1);

    for (QAction *a : w.findChildren<QAction *>())
        if (a->shortcut() == QKeySequence(QStringLiteral("Ctrl+T"))) a->trigger();
    app.processEvents();
    w.grab().save(QStringLiteral("outline.png"));

    Overlay *ov = w.findChild<Overlay *>();
    ov->dismiss();
    // Exercise the real peek path through the signal MainWindow listens on.
    emit v->linkPeekRequested(2);
    app.processEvents();
    w.grab().save(QStringLiteral("peek.png"));
    std::printf("peek presented: %s\n", ov->isPresented() ? "yes" : "no");
    return hitX >= 0 && target == 2 ? 0 : 1;
}
