#include "mainwindow.h"
#include "pageview.h"
#include "overlay.h"
#include "document.h"
#include <QApplication>
#include <QAction>
#include <QListWidget>
#include <QLabel>
#include <QKeySequence>
#include <cstdio>
#include <QEventLoop>
#include <QTimer>
// Jumps are eased since Z7, so landing is not synchronous with asking.
static void settle(int ms = 320) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }

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
int main(int argc, char **argv) {
    QApplication app(argc, argv);

    std::printf("\n--- outline extraction ---\n");
    Document d;
    check(d.openPath(QStringLiteral("outline.pdf")) == LoadStatus::Ok, "outline.pdf opened");
    const QVector<OutlineEntry> o = d.outline();
    for (const OutlineEntry &e : o)
        std::printf("      depth=%d page=%d  \"%s\"\n", e.depth, e.page, qPrintable(e.title));
    check(o.size() == 3, QStringLiteral("three outline entries (got %1)").arg(o.size()));
    check(o.size() > 0 && o[0].title == "Chapter One" && o[0].depth == 0 && o[0].page == 0,
          "Chapter One at depth 0 -> page 0");
    check(o.size() > 1 && o[1].title == "Chapter Two" && o[1].depth == 0 && o[1].page == 1,
          "Chapter Two at depth 0 -> page 1");
    check(o.size() > 2 && o[2].title == "Section 2.1" && o[2].depth == 1 && o[2].page == 2,
          "Section 2.1 NESTED at depth 1 -> page 2");

    Document plain;
    plain.openPath(QStringLiteral("test.pdf"));
    check(plain.outline().isEmpty(), "a document with no outline reports none");

    std::printf("\n--- link extraction, unrotated and rotated ---\n");
    const QVector<PageLink> l0 = d.pageLinks(0, Poppler::Page::Rotate0);
    check(l0.size() == 1, QStringLiteral("one internal link on page 1 (got %1)").arg(l0.size()));
    check(l0.size() == 1 && l0[0].page == 2, "link targets page 3 (index 2)");
    if (l0.size() == 1)
        std::printf("      rot0   area = %.0f,%.0f %.0fx%.0f\n", l0[0].area.x(), l0[0].area.y(),
                    l0[0].area.width(), l0[0].area.height());
    const QVector<PageLink> l90 = d.pageLinks(0, Poppler::Page::Rotate90);
    check(l90.size() == 1, "link still found at 90 degrees");
    if (l90.size() == 1) {
        std::printf("      rot90  area = %.0f,%.0f %.0fx%.0f\n", l90[0].area.x(), l90[0].area.y(),
                    l90[0].area.width(), l90[0].area.height());
        // A 90-degree turn swaps the rect's extents.
        check(qAbs(l90[0].area.width() - l0[0].area.height()) < 1.0 &&
              qAbs(l90[0].area.height() - l0[0].area.width()) < 1.0,
              "rotated link area has its extents swapped");
    }
    check(d.pageLinks(1, Poppler::Page::Rotate0).isEmpty(), "page 2 has no links");

    std::printf("\n--- outline overlay ---\n");
    MainWindow w; w.resize(950, 780); w.show();
    w.openPath(QStringLiteral("outline.pdf"));
    Overlay *ov = w.findChild<Overlay *>();
    QAction *tocA = byShortcut(&w, "Ctrl+T");
    check(tocA != nullptr, "Ctrl+T registered");
    tocA->trigger();
    check(ov->isPresented(), "Ctrl+T presents the outline");
    QListWidget *list = ov->findChild<QListWidget *>();
    check(list && list->count() == 3, "outline overlay lists three entries");
    check(list && list->item(2)->text().startsWith("    "), "nested entry is indented");

    std::printf("\n--- outline jump lands right when zoomed and rotated ---\n");
    PageView *v = w.findChild<PageView *>();
    v->setZoom(2.5);
    v->rotateClockwise();
    list->setCurrentRow(2);
    emit list->itemActivated(list->item(2));
    app.processEvents();
    settle();
    check(!ov->isPresented(), "choosing an entry dismissed the overlay");
    check(v->currentPage() == 2,
          QStringLiteral("jumped to page 3 at 250%% rotated 90 (got page %1)").arg(v->currentPage() + 1));

    std::printf("\n--- a document with no outline says so ---\n");
    w.openPath(QStringLiteral("test.pdf"));
    tocA->trigger();
    check(ov->isPresented(), "overlay still opens with no outline");
    QLabel *msg = ov->findChild<QLabel *>();
    bool said = false;
    for (QLabel *lb : ov->findChildren<QLabel *>())
        if (lb->text().contains("no table of contents")) said = true;
    check(said, "it says the document has no table of contents");
    check(ov->findChildren<QListWidget *>().isEmpty(), "and shows no empty list");
    (void)msg;

    std::printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", fails);
    return fails;
}
