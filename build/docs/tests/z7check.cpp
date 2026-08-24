#include "mainwindow.h"
#include "pageview.h"
#include "overlay.h"
#include <QApplication>
#include <QAction>
#include <QScrollBar>
#include <QKeySequence>
#include <QMimeData>
#include <QUrl>
#include <QDragEnterEvent>
#include <QToolBar>
#include <QFileInfo>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
using namespace mergen;
int fails = 0;
static void check(bool ok, const QString &what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", qPrintable(what)); if (!ok) fails++;
}
static QAction *byShortcut(QObject *r, const char *k) {
    for (QAction *a : r->findChildren<QAction *>())
        if (a->shortcut() == QKeySequence(QString::fromUtf8(k))) return a;
    return nullptr;
}
static void spin(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    MainWindow w; w.resize(900, 800); w.show();
    w.openPath(QStringLiteral("outline.pdf"));
    app.processEvents();
    PageView *v = w.findChild<PageView *>();
    QScrollBar *bar = v->verticalScrollBar();

    std::printf("\n--- a jump is eased, not snapped ---\n");
    bar->setValue(0); app.processEvents();
    v->scrollToPage(2);
    const int immediately = bar->value();
    std::printf("      right after the call: %d\n", immediately);
    check(immediately == 0, "the bar has not teleported");
    spin(60);
    const int midway = bar->value();
    std::printf("      60ms in:              %d\n", midway);
    check(midway > 0, "it is moving");
    spin(300);
    std::printf("      settled:              %d  (target region)\n", bar->value());
    check(midway < bar->value(), "and it kept going");
    check(v->currentPage() == 2, "it landed on page 3");

    std::printf("\n--- a second jump mid-flight picks up from where it is ---\n");
    bar->setValue(0); app.processEvents();
    v->scrollToPage(2);
    spin(60);
    const int interrupted = bar->value();
    v->scrollToPage(0);
    spin(20);
    // Restarting from the live value means no snap back to 0 on the first frame.
    std::printf("      interrupted at %d, one frame later %d\n", interrupted, bar->value());
    check(bar->value() > 0 && bar->value() <= interrupted + 5,
          "it eased back from where it was, it did not snap");
    spin(320);

    std::printf("\n--- Qt's effects flag is unusable as a gate here ---\n");
    std::printf("      isEffectEnabled(UI_General) = %s on this platform\n",
                QApplication::isEffectEnabled(Qt::UI_General) ? "true" : "false");
    check(!QApplication::isEffectEnabled(Qt::UI_General),
          "Qt reports effects OFF with no desktop environment (so it cannot gate motion)");

    std::printf("\n--- presentation mode ---\n");
    QAction *f5 = byShortcut(&w, "F5");
    check(f5 != nullptr, "F5 registered");
    check(!v->isPresenting(), "starts off");
    f5->trigger(); app.processEvents();
    check(v->isPresenting(), "F5 turns it on");
    check(!w.findChild<QToolBar *>()->isVisible(), "the toolbar is withdrawn");
    check(w.isFullScreen(), "the window went fullscreen");
    const QColor corner = v->viewport()->grab().toImage().pixelColor(2, 2);
    std::printf("      surround now %s\n", qPrintable(corner.name()));
    check(corner == QColor(Qt::black), "the surround is black");
    QAction *esc = byShortcut(&w, "Esc");
    esc->trigger(); app.processEvents();
    check(!v->isPresenting(), "Esc leaves presentation");
    check(w.findChild<QToolBar *>()->isVisible(), "the toolbar is back");

    std::printf("\n--- Esc order: overlay, then search, then presentation ---\n");
    f5->trigger(); app.processEvents();
    byShortcut(&w, "Ctrl+I")->trigger();
    Overlay *ov = w.findChild<Overlay *>();
    check(ov->isPresented(), "an overlay opened while presenting");
    esc->trigger(); app.processEvents();
    check(!ov->isPresented() && v->isPresenting(),
          "Esc closed the overlay and LEFT presentation running");
    esc->trigger(); app.processEvents();
    check(!v->isPresenting(), "a second Esc left presentation");

    std::printf("\n--- the empty window takes a dropped PDF ---\n");
    MainWindow e; e.resize(700, 600); e.show();
    check(e.acceptDrops(), "the window accepts drops");
    QMimeData *mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(QFileInfo(QStringLiteral("test.pdf")).absoluteFilePath())});
    QDragEnterEvent enter(QPoint(300, 300), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&e, &enter);
    check(enter.isAccepted(), "a dragged PDF is accepted");
    QDropEvent drop(QPointF(300, 300), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&e, &drop);
    app.processEvents();
    PageView *ev = e.findChild<PageView *>();
    check(ev->currentPage() >= 0, "dropping it opened the document");

    QMimeData *bad = new QMimeData;
    bad->setUrls({QUrl::fromLocalFile(QStringLiteral("/etc/hostname"))});
    QDragEnterEvent badEnter(QPoint(10,10), Qt::CopyAction, bad, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&e, &badEnter);
    check(!badEnter.isAccepted(), "a non-PDF is refused at the door");

    std::printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", fails);
    return fails;
}
