#include "mainwindow.h"
#include "pageview.h"
#include "overlay.h"
#include "document.h"
#include <QApplication>
#include <QAction>
#include <QLineEdit>
#include <QListWidget>
#include <QKeySequence>
#include <QKeyEvent>
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
static QLineEdit *cmdEdit(Overlay *ov) {
    for (QLineEdit *e : ov->findChildren<QLineEdit *>())
        if (e->accessibleName() == "Command") return e;
    return nullptr;
}
static QStringList rows(QListWidget *l) {
    QStringList out; for (int i=0;i<l->count();++i) out << l->item(i)->text(); return out;
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    MainWindow w; w.resize(950, 780); w.show();
    Overlay *ov = w.findChild<Overlay *>();
    PageView *v = w.findChild<PageView *>();
    QAction *k = byShortcut(&w, "Ctrl+K");

    std::printf("\n--- with no document ---\n");
    check(k != nullptr, "Ctrl+K registered");
    k->trigger();
    check(ov->isPresented(), "Ctrl+K opens with no document (Open must stay reachable)");
    QListWidget *list = ov->findChild<QListWidget *>();
    check(list && rows(list).contains("Open"), "Open is listed with no document");
    check(list && !rows(list).contains("Print"), "Print is NOT listed (disabled without a document)");
    ov->dismiss();

    std::printf("\n--- page number ---\n");
    w.openPath(QStringLiteral("test.pdf"));
    k->trigger();
    QLineEdit *e = cmdEdit(ov); list = ov->findChild<QListWidget *>();
    check(e != nullptr, "command field present");
    e->setText(QStringLiteral("3"));
    check(rows(list).value(0) == "Go to page 3",
          QStringLiteral("typing 3 offers \"Go to page 3\" first (got \"%1\")").arg(rows(list).value(0)));
    e->setText(QStringLiteral("999"));
    check(!rows(list).value(0).startsWith("Go to page"),
          "a page the document does not have is not offered");
    e->setText(QStringLiteral("3"));
    emit e->returnPressed();
    app.processEvents();
    check(!ov->isPresented(), "Enter dismissed the overlay");
    settle();
    check(v->currentPage() == 2, QStringLiteral("jumped to page 3 (got %1)").arg(v->currentPage()+1));

    std::printf("\n--- search term ---\n");
    k->trigger(); e = cmdEdit(ov); list = ov->findChild<QListWidget *>();
    e->setText(QStringLiteral("needle"));
    check(rows(list).value(0).contains("needle"),
          QStringLiteral("typing text offers a find (got \"%1\")").arg(rows(list).value(0)));
    emit e->returnPressed();
    app.processEvents();
    check(!ov->isPresented(), "Enter dismissed the overlay");
    QLineEdit *searchEdit = nullptr;
    for (QLineEdit *le : w.findChildren<QLineEdit *>())
        if (le->accessibleName() != "Page number" && le->accessibleName() != "Command") searchEdit = le;
    check(searchEdit && searchEdit->isVisible(), "the search bar opened");
    check(searchEdit && searchEdit->text() == "needle", "and carries the term");

    std::printf("\n--- action by name ---\n");
    k->trigger(); e = cmdEdit(ov); list = ov->findChild<QListWidget *>();
    e->setText(QStringLiteral("rot"));
    std::printf("      rows: %s\n", qPrintable(rows(list).join(" | ")));
    check(rows(list).value(0) == "Rotate",
          QStringLiteral("typing \"rot\" ranks Rotate FIRST, above the find"));
    const auto before = v->rotation();
    emit e->returnPressed();
    app.processEvents();
    check(v->rotation() != before, "running it rotated the document");

    std::printf("\n--- Up and Down move the list while typing ---\n");
    k->trigger(); e = cmdEdit(ov); list = ov->findChild<QListWidget *>();
    e->setText(QString());
    check(list->currentRow() == 0, "starts on the first row");
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(e, &down);
    check(list->currentRow() == 1, QStringLiteral("Down moved to row 2 (got %1)").arg(list->currentRow()));
    QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QApplication::sendEvent(e, &up);
    check(list->currentRow() == 0, "Up moved back to row 1");
    check(e->hasFocus(), "the field kept focus throughout");

    std::printf("\n--- the old routes still work ---\n");
    ov->dismiss();
    QLineEdit *pageEdit = nullptr;
    for (QLineEdit *le : w.findChildren<QLineEdit *>())
        if (le->accessibleName() == "Page number") pageEdit = le;
    pageEdit->setText(QStringLiteral("2"));
    emit pageEdit->returnPressed();
    app.processEvents();
    settle();
    check(v->currentPage() == 1, "the page counter still jumps");
    QAction *searchA = nullptr;
    for (QAction *a : w.findChildren<QAction *>()) if (a->text() == "Search") searchA = a;
    searchA->trigger();
    check(searchEdit->isVisible(), "the search button still opens the bar");

    std::printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", fails);
    return fails;
}
