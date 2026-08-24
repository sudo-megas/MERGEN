#include "document.h"
#include <QApplication>
#include <QElapsedTimer>
#include <cstdio>
using namespace mergen;
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    Document d; d.openPath(QStringLiteral("colour.pdf"));
    std::printf("%-10s %-14s %s\n", "zoom", "render (ms)", "frames at 60fps budget (16.7ms)");
    for (double z : {0.5, 1.0, 1.5, 2.5, 4.0}) {
        QElapsedTimer t; t.start();
        int n = 0;
        while (t.elapsed() < 300) { (void)d.renderPage(0, z, Poppler::Page::Rotate0); ++n; }
        const double ms = double(t.elapsed()) / n;
        std::printf("%-10.0f %-14.1f %s\n", z*100, ms, ms < 16.7 ? "fits" : "MISSES BUDGET");
    }
    return 0;
}
