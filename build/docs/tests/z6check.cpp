#include "mainwindow.h"
#include "pageview.h"
#include "document.h"
#include <QApplication>
#include <QImage>
#include <QColor>
#include <cstdio>
using namespace mergen;
int fails = 0;
static void check(bool ok, const QString &what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", qPrintable(what)); if (!ok) fails++;
}
// Is there a strongly yellow pixel anywhere? That is the highlight.
static bool hasYellow(const QImage &im) {
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x) {
            const QColor c(im.pixel(x, y));
            if (c.red() > 200 && c.green() > 200 && c.blue() < 90) return true;
        }
    return false;
}
static bool hasBlueish(const QImage &im) {
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x) {
            const QColor c(im.pixel(x, y));
            if (c.blue() > 150 && c.blue() - c.red() > 60) return true;
        }
    return false;
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    Document d;
    check(d.openPath(QStringLiteral("annot.pdf")) == LoadStatus::Ok, "annot.pdf opened");

    std::printf("\n--- annotations render at all ---\n");
    check(hasYellow(d.renderPage(0, 1.0, Poppler::Page::Rotate0)), "highlight drawn at 100%");
    check(hasBlueish(d.renderPage(0, 1.0, Poppler::Page::Rotate0)), "square drawn at 100%");

    std::printf("\n--- and survive zoom and rotation ---\n");
    check(hasYellow(d.renderPage(0, 2.5, Poppler::Page::Rotate0)), "highlight drawn at 250%");
    check(hasYellow(d.renderPage(0, 1.0, Poppler::Page::Rotate90)), "highlight drawn rotated 90");
    check(hasYellow(d.renderPage(0, 2.5, Poppler::Page::Rotate90)), "highlight drawn at 250% rotated 90");
    check(hasYellow(d.renderPage(0, 1.0, Poppler::Page::Rotate270)), "highlight drawn rotated 270");

    std::printf("\n--- geometry follows the page, not the viewport ---\n");
    const QImage flat = d.renderPage(0, 1.0, Poppler::Page::Rotate0);
    const QImage turned = d.renderPage(0, 1.0, Poppler::Page::Rotate90);
    std::printf("      rot0 %dx%d   rot90 %dx%d\n", flat.width(), flat.height(),
                turned.width(), turned.height());
    check(qAbs(flat.width() - turned.height()) <= 1 && qAbs(flat.height() - turned.width()) <= 1,
          "the rotated page has its extents swapped");

    std::printf("\n--- night mode keeps the highlight legible ---\n");
    MainWindow w; w.resize(700, 800); w.show();
    w.openPath(QStringLiteral("annot.pdf"));
    PageView *v = w.findChild<PageView *>();
    app.processEvents();
    const QImage day = v->viewport()->grab().toImage();
    v->setNightMode(true); app.processEvents();
    const QImage night = v->viewport()->grab().toImage();
    check(hasYellow(day), "highlight visible in day mode");
    check(hasYellow(night), "highlight STILL yellow in night mode (hue preserved)");
    check(day != night, "the page did change");

    std::printf("\n--- properties report the markup ---\n");
    const DocumentProperties p = d.properties();
    QString marks;
    for (const Property &r : p.rows) if (r.name == "Annotations") marks = r.value;
    std::printf("      Annotations = \"%s\"\n", qPrintable(marks));
    check(marks == "3", "three annotations counted (highlight, underline, square)");

    Document plain; plain.openPath(QStringLiteral("test.pdf"));
    QString none;
    for (const Property &r : plain.properties().rows) if (r.name == "Annotations") none = r.value;
    check(none.isEmpty(), "an unannotated document reports no annotation row at all");

    Document linked; linked.openPath(QStringLiteral("outline.pdf"));
    QString links;
    for (const Property &r : linked.properties().rows) if (r.name == "Annotations") links = r.value;
    check(links.isEmpty(), "a document whose only annots are LINKS reports none (links are structure)");

    std::printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", fails);
    return fails;
}
