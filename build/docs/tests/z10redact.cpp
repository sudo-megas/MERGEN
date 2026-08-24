#include "redact.h"
#include <QCoreApplication>
#include <QRectF>
#include <cstdio>
using namespace mergen;
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    // test.pdf draws three lines with origins at (72,700) (72,660) (72,640).
    // Redact only the middle one.
    const Redact::Result r = Redact::run(QStringLiteral("test.pdf"),
                                         QStringLiteral("redacted.pdf"), 0,
                                         QRectF(60, 650, 460, 25),
                                         QStringLiteral("searchable haystack needle"));
    std::printf("ok=%d  error=\"%s\"\n", int(r.ok), qPrintable(r.error));
    return r.ok ? 0 : 1;
}
