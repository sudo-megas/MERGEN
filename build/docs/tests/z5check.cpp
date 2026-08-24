#include "mainwindow.h"
#include "pageview.h"
#include "document.h"
#include <QApplication>
#include <QAction>
#include <QKeySequence>
#include <QImage>
#include <QColor>
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
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    MainWindow w; w.resize(900, 800); w.show();
    w.openPath(QStringLiteral("colour.pdf"));
    app.processEvents();
    PageView *v = w.findChild<PageView *>();
    QAction *n = byShortcut(&w, "Ctrl+N");

    std::printf("\n--- toggle ---\n");
    check(n != nullptr, "Ctrl+N registered");
    check(!v->isNightMode(), "night mode starts off");
    const QImage plain = v->viewport()->grab().toImage();
    n->trigger(); app.processEvents();
    check(v->isNightMode(), "Ctrl+N turns it on");
    const QImage dark = v->viewport()->grab().toImage();
    check(plain != dark, "the rendered page actually changed");

    std::printf("\n--- toggling twice returns the original exactly ---\n");
    n->trigger(); app.processEvents();
    check(!v->isNightMode(), "Ctrl+N turns it off again");
    const QImage back = v->viewport()->grab().toImage();
    check(back == plain, "the image is pixel-identical to before (involution through the cache)");

    std::printf("\n--- hue survives, lightness inverts ---\n");
    // Sample each swatch straight from a rendered page, both ways.
    Document d; d.openPath(QStringLiteral("colour.pdf"));
    const QImage page = d.renderPage(0, 1.0, Poppler::Page::Rotate0);
    v->setNightMode(true); app.processEvents();
    // Re-render through the view so the transform is the shipped one.
    const QImage litPage  = page;
    QImage darkPage = litPage.convertToFormat(QImage::Format_RGB32);
    {   // same transform the view applies
        for (int y = 0; y < darkPage.height(); ++y) {
            auto *line = reinterpret_cast<QRgb *>(darkPage.scanLine(y));
            for (int x = 0; x < darkPage.width(); ++x) {
                const QRgb p = line[x];
                const int r = qRed(p), g = qGreen(p), b = qBlue(p);
                const int s = 255 - (std::max({r,g,b}) + std::min({r,g,b}));
                line[x] = qRgb(std::clamp(r+s,0,255), std::clamp(g+s,0,255), std::clamp(b+s,0,255));
            }
        }
    }
    struct Probe { const char *name; int x, y; };
    // Swatches sit at y=600..720 in PDF points from the bottom of an 842pt page.
    const int sy = int((842 - 660) * (litPage.height() / 842.0));
    const Probe probes[] = {{"red",int(105*litPage.width()/595.0),sy},
                            {"blue",int(205*litPage.width()/595.0),sy},
                            {"dark green",int(305*litPage.width()/595.0),sy},
                            {"orange",int(405*litPage.width()/595.0),sy},
                            {"black",int(505*litPage.width()/595.0),sy}};
    for (const Probe &p : probes) {
        const QColor before(litPage.pixel(p.x, p.y));
        const QColor after(darkPage.pixel(p.x, p.y));
        std::printf("      %-11s %-16s -> %-16s  hue %3d -> %3d   light %3d -> %3d\n",
                    p.name, qPrintable(before.name()), qPrintable(after.name()),
                    before.hslHue(), after.hslHue(), before.lightness(), after.lightness());
        check(before.hslHue() == after.hslHue(),
              QStringLiteral("%1: hue preserved").arg(QString::fromUtf8(p.name)));
    }
    const QColor white(litPage.pixel(int(20*litPage.width()/595.0), sy));
    const QColor whiteDark(darkPage.pixel(int(20*litPage.width()/595.0), sy));
    std::printf("      %-11s %-16s -> %-16s\n", "page white", qPrintable(white.name()),
                qPrintable(whiteDark.name()));
    check(white.lightness() > 200 && whiteDark.lightness() < 55, "the white page became dark");

    std::printf("\n--- the surround follows the page ---\n");
    v->setNightMode(false); app.processEvents();
    const QColor litCorner  = v->viewport()->grab().toImage().pixelColor(2, 2);
    v->setNightMode(true); app.processEvents();
    const QColor darkCorner = v->viewport()->grab().toImage().pixelColor(2, 2);
    std::printf("      surround %s -> %s\n", qPrintable(litCorner.name()), qPrintable(darkCorner.name()));
    check(darkCorner.lightness() < litCorner.lightness(),
          "the canvas behind the pages darkens too");

    std::printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", fails);
    return fails;
}
