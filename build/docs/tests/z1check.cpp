#include "mainwindow.h"
#include "pageview.h"
#include <QApplication>
#include <QAction>
#include <QToolButton>
#include <QLineEdit>
#include <QDebug>
#include <cstdio>
#include <QTimer>
using namespace mergen;

static QAction *byText(QObject *root, const QString &t) {
    for (QAction *a : root->findChildren<QAction *>())
        if (a->text() == t) return a;
    return nullptr;
}
int fails = 0;
static void check(bool ok, const QString &what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) fails++;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    MainWindow w;
    w.show();

    QAction *zin = byText(&w, "Zoom in");
    QAction *zout = byText(&w, "Zoom out");
    QAction *print = byText(&w, "Print");
    QAction *search = byText(&w, "Search");

    std::printf("\n--- with no document ---\n");
    check(zin && !zin->isEnabled(), "Zoom in disabled");
    check(print && !print->isEnabled(), "Print disabled");
    check(search && !search->isEnabled(), "Search disabled");

    std::printf("\n--- accessibility / icons ---\n");
    bool allNamed = true, allIcons = true, allTips = true;
    for (const QString &n : {"Open","Zoom out","Zoom in","Fit width","Fit page","Rotate","Search","Print"}) {
        QAction *a = byText(&w, n);
        if (!a) { allNamed = false; continue; }
        // The action text must be a real word, never a private-use code point.
        if (a->text().isEmpty() || a->text().at(0).unicode() >= 0xE000) allNamed = false;
        if (a->icon().isNull()) allIcons = false;
        if (a->toolTip().isEmpty()) allTips = false;
    }
    check(allNamed, "every action's text is a real word, not a PUA code point");
    check(allIcons, "every action carries a non-null icon");
    check(allTips,  "every action carries a tooltip");
    QAction *fw = byText(&w, "Fit width");
    check(fw && fw->toolTip().contains("F"), QString("Fit width tooltip names its key: \"%1\"").arg(fw?fw->toolTip():""));
    check(fw && fw->shortcut().isEmpty(), "Fit width registers NO window-wide shortcut (would eat 'f' in search)");

    std::printf("\n--- open document ---\n");
    w.openPath(QStringLiteral("test.pdf"));
    check(zin && zin->isEnabled(), "Zoom in enabled with a document");
    check(print && print->isEnabled(), "Print enabled with a document");

    std::printf("\n--- zoom to the 1000%% ceiling ---\n");
    for (int i = 0; i < 200 && zin->isEnabled(); ++i) zin->trigger();
    PageView *v = w.findChild<PageView *>();
    std::printf("    final zoom: %.4f\n", v ? v->zoom() : -1.0);
    check(v && qAbs(v->zoom() - PageView::kMaxZoom) < 1e-6, "zoom reached exactly 1000%");
    check(zin && !zin->isEnabled(), "Zoom in DISABLED at the ceiling");
    check(zout && zout->isEnabled(), "Zoom out still enabled at the ceiling");

    std::printf("\n--- zoom to the 10%% floor ---\n");
    for (int i = 0; i < 400 && zout->isEnabled(); ++i) zout->trigger();
    std::printf("    final zoom: %.4f\n", v ? v->zoom() : -1.0);
    check(v && qAbs(v->zoom() - PageView::kMinZoom) < 1e-6, "zoom reached exactly 10%");
    check(zout && !zout->isEnabled(), "Zoom out DISABLED at the floor");
    check(zin && zin->isEnabled(), "Zoom in re-enabled at the floor");

    std::printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", fails);
    return fails;
}
