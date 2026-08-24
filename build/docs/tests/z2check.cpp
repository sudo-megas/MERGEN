#include "mainwindow.h"
#include "pageview.h"
#include "overlay.h"
#include "document.h"
#include <QApplication>
#include <QAction>
#include <QLineEdit>
#include <QKeySequence>
#include <cstdio>
using namespace mergen;

int fails = 0;
static void check(bool ok, const QString &what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) fails++;
}
static QAction *byShortcut(QObject *root, const QKeySequence &k) {
    for (QAction *a : root->findChildren<QAction *>())
        if (a->shortcut() == k) return a;
    return nullptr;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    MainWindow w;
    w.resize(1000, 800);
    w.show();

    Overlay *ov = w.findChild<Overlay *>();
    QAction *props = byShortcut(&w, QKeySequence(QStringLiteral("Ctrl+I")));
    QAction *esc = byShortcut(&w, QKeySequence(QStringLiteral("Esc")));

    std::printf("\n--- wiring ---\n");
    check(ov != nullptr, "overlay exists");
    check(props != nullptr, "Ctrl+I action registered");
    check(esc != nullptr, "Esc action registered");
    check(ov && !ov->isPresented(), "overlay starts hidden");

    std::printf("\n--- with no document ---\n");
    props->trigger();
    check(ov && !ov->isPresented(), "Ctrl+I with no document presents nothing");

    std::printf("\n--- present over a document ---\n");
    w.openPath(QStringLiteral("test.pdf"));
    QLineEdit *pageEdit = nullptr, *searchEdit = nullptr;
    for (QLineEdit *e : w.findChildren<QLineEdit *>()) {
        if (e->accessibleName() == "Page number") pageEdit = e; else searchEdit = e;
    }
    check(pageEdit && searchEdit && pageEdit != searchEdit,
          "page counter and search field told apart by accessible name");
    if (pageEdit) pageEdit->setFocus(Qt::OtherFocusReason);
    QWidget *before = QApplication::focusWidget();
    props->trigger();
    check(ov && ov->isPresented(), "Ctrl+I presents the overlay");
    check(ov && ov->isVisible() && ov->geometry() == ov->parentWidget()->rect(),
          "overlay covers the page view exactly");

    std::printf("\n--- Esc dismisses, focus goes back ---\n");
    esc->trigger();
    check(ov && !ov->isPresented(), "Esc dismisses the overlay");
    check(QApplication::focusWidget() == before,
          QStringLiteral("focus returned to where it was found"));

    std::printf("\n--- overlay never survives a document close ---\n");
    props->trigger();
    check(ov && ov->isPresented(), "overlay presented again");
    w.openPath(QStringLiteral("does-not-exist.pdf"));   // -> showError -> closeDocument
    check(ov && !ov->isPresented(), "closing the document dismissed the overlay");

    std::printf("\n--- Esc still reaches the search bar when nothing is over it ---\n");
    w.openPath(QStringLiteral("test.pdf"));
    QAction *search = nullptr;
    for (QAction *a : w.findChildren<QAction *>()) if (a->text() == "Search") search = a;
    search->trigger();
    check(searchEdit->isVisible(), "search bar opened");
    check(pageEdit->isVisible(), "page counter is visible (and is NOT the search field)");
    esc->trigger();
    check(!searchEdit->isVisible(), "Esc closed the search bar (dispatcher fell through)");
    check(pageEdit->isVisible(), "page counter untouched by Esc");

    std::printf("\n--- a document that carries JavaScript says so ---\n");
    Document d;
    check(d.openPath(QStringLiteral("evil.pdf")) == LoadStatus::Ok, "evil.pdf opened");
    const DocumentProperties p = d.properties();
    bool js = false, forms = false;
    for (const QString &warn : p.warnings) {
        if (warn.contains(QStringLiteral("JavaScript"))) js = true;
        if (warn.contains(QStringLiteral("form"), Qt::CaseInsensitive)) forms = true;
    }
    for (const QString &warn : p.warnings) std::printf("      warning: %s\n", qPrintable(warn));
    check(js, "embedded JavaScript is reported");
    check(forms, "form fields are reported");

    Document clean;
    clean.openPath(QStringLiteral("test.pdf"));
    check(clean.properties().warnings.isEmpty(), "an ordinary document raises no warnings");
    bool hasVersion = false, hasPages = false;
    for (const Property &r : clean.properties().rows) {
        if (r.name == "PDF version") hasVersion = true;
        if (r.name == "Pages" && r.value == "3") hasPages = true;
    }
    check(hasVersion, "PDF version reported");
    check(hasPages, "page count reported and correct");

    std::printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", fails);
    return fails;
}
