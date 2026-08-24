#include "mainwindow.h"
#include "pageview.h"
#include "overlay.h"
#include "document.h"
#include <QApplication>
#include <QAction>
#include <QScrollBar>
#include <QListWidget>
#include <QKeySequence>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
using namespace mergen;
int fails = 0;
static void check(bool ok, const QString &what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", qPrintable(what)); if (!ok) fails++;
}
static void settle(int ms = 320) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }
static QAction *byShortcut(QObject *r, const char *k) {
    for (QAction *a : r->findChildren<QAction *>())
        if (a->shortcut() == QKeySequence(QString::fromUtf8(k))) return a;
    return nullptr;
}
static QString portalFile() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation)
           + QStringLiteral("/mergen/portals.toml");
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QFile::remove(portalFile());

    std::printf("\n--- content hash identifies a document by its bytes ---\n");
    Document a, b;
    a.openPath(QStringLiteral("outline.pdf"));
    QFile::remove(QStringLiteral("renamed.pdf"));
    QFile::copy(QStringLiteral("outline.pdf"), QStringLiteral("renamed.pdf"));
    b.openPath(QStringLiteral("renamed.pdf"));
    std::printf("      outline.pdf  %s\n", qPrintable(a.contentHash().left(16)));
    std::printf("      renamed.pdf  %s\n", qPrintable(b.contentHash().left(16)));
    check(!a.contentHash().isEmpty(), "a hash is produced");
    check(a.contentHash() == b.contentHash(), "the same bytes under a new name hash the same");
    Document c; c.openPath(QStringLiteral("outline-v2.pdf"));
    check(a.contentHash() != c.contentHash(), "different bytes hash differently");

    MainWindow w; w.resize(1200, 820); w.show();
    w.openPath(QStringLiteral("outline.pdf"));
    app.processEvents();
    PageView *v = w.findChild<PageView *>();

    std::printf("\n--- compare marks only what differs ---\n");
    check(!w.isComparing(), "not comparing to begin with");
    w.enterCompare(QDir::current().absoluteFilePath(QStringLiteral("outline.pdf")));
    app.processEvents();
    check(w.isComparing(), "entered compare");
    v->viewport()->grab();          // marks are computed when a page is painted
    check(!v->hasDiffBands(), "a document compared with ITSELF marks nothing");
    w.leaveCompare();
    check(!w.isComparing(), "left compare");

    w.enterCompare(QDir::current().absoluteFilePath(QStringLiteral("outline-v2.pdf")));
    app.processEvents();
    check(w.isComparing(), "entered compare against the changed copy");
    v->scrollToPage(1);
    { QEventLoop l; QTimer::singleShot(400,&l,&QEventLoop::quit); l.exec(); }
    v->viewport()->grab();
    check(v->hasDiffBands(), "differences are marked");

    std::printf("\n--- both views scroll together ---\n");
    QList<PageView *> views = w.findChildren<PageView *>();
    check(views.size() == 2, QStringLiteral("two views on screen (got %1)").arg(views.size()));
    if (views.size() == 2) {
        views[0]->verticalScrollBar()->setValue(400);
        app.processEvents();
        std::printf("      left %d   right %d\n", views[0]->verticalScrollBar()->value(),
                    views[1]->verticalScrollBar()->value());
        check(views[1]->verticalScrollBar()->value() == 400, "the second view followed the first");
        views[1]->verticalScrollBar()->setValue(120);
        app.processEvents();
        check(views[0]->verticalScrollBar()->value() == 120, "and the first followed the second");
    }
    w.leaveCompare();
    app.processEvents();
    check(w.findChildren<PageView *>().size() == 1, "leaving compare returns to one view");
    check(!v->hasDiffBands(), "and clears the marks");

    std::printf("\n--- portals ---\n");
    QAction *mark = byShortcut(&w, "Ctrl+M");
    QAction *follow = byShortcut(&w, "Ctrl+J");
    check(mark && follow, "Ctrl+M and Ctrl+J registered");
    w.openPath(QStringLiteral("outline.pdf"));
    v->scrollToPage(0); settle();
    mark->trigger();                       // one end on page 1
    v->scrollToPage(2); settle();
    mark->trigger();                       // other end on page 3
    check(QFile::exists(portalFile()), "portals.toml was written");
    v->scrollToPage(0); settle();
    follow->trigger(); settle();
    check(v->currentPage() == 2, QStringLiteral("Ctrl+J jumped to the far end (page %1)").arg(v->currentPage()+1));
    follow->trigger(); settle();
    check(v->currentPage() == 0, "and back again from the other side");

    std::printf("\n--- a portal survives its document being renamed ---\n");
    {
        QFile f(portalFile()); f.open(QIODevice::ReadOnly);
        const QString body = QString::fromUtf8(f.readAll());
        check(body.contains(QStringLiteral("hash =")), "the file records a hash, not just a path");
    }
    MainWindow w2; w2.resize(900, 700); w2.show();
    w2.openPath(QStringLiteral("renamed.pdf"));     // same bytes, different name
    PageView *v2 = w2.findChild<PageView *>();
    app.processEvents();
    v2->scrollToPage(0); settle();
    byShortcut(&w2, "Ctrl+J")->trigger(); settle();
    check(v2->currentPage() == 2,
          QStringLiteral("the portal still works under the new name (page %1)").arg(v2->currentPage()+1));

    std::printf("\n--- a portal to a missing document is kept, and says so ---\n");
    QFile::remove(QStringLiteral("gone.pdf"));
    QFile::copy(QStringLiteral("test.pdf"), QStringLiteral("gone.pdf"));
    MainWindow w3; w3.resize(900, 700); w3.show();
    w3.openPath(QStringLiteral("gone.pdf"));
    PageView *v3 = w3.findChild<PageView *>();
    byShortcut(&w3, "Ctrl+M")->trigger();
    w3.openPath(QStringLiteral("outline.pdf"));
    v3->scrollToPage(1); settle();
    byShortcut(&w3, "Ctrl+M")->trigger();
    QFile::remove(QStringLiteral("gone.pdf"));
    MainWindow w4; w4.resize(900, 700); w4.show();
    w4.openPath(QStringLiteral("outline.pdf"));
    PageView *v4 = w4.findChild<PageView *>();
    app.processEvents();
    v4->scrollToPage(1); settle();
    byShortcut(&w4, "Ctrl+J")->trigger(); settle();
    check(v4->currentPage() == 1, "it did not navigate anywhere");
    {   // the portal is still on disk
        QFile f(portalFile()); f.open(QIODevice::ReadOnly);
        check(QString::fromUtf8(f.readAll()).count(QStringLiteral("[[portal]]")) == 2,
              "both portals are still recorded, none silently dropped");
    }

    std::printf("\n--- portals are listed in the command overlay ---\n");
    byShortcut(&w4, "Ctrl+K")->trigger();
    Overlay *ov = w4.findChild<Overlay *>();
    QListWidget *cmd = ov->findChild<QListWidget *>();
    bool listed = false;
    for (int i = 0; i < cmd->count(); ++i)
        if (cmd->item(i)->text() == "Portals") listed = true;
    check(listed, "\"Portals\" is offered in the command overlay");

    QFile::remove(QStringLiteral("renamed.pdf"));
    std::printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", fails);
    return fails;
}
