#include "redact.h"
#include <QCoreApplication>
#include <QProcess>
#include <QFile>
#include <QRectF>
#include <cstdio>
using namespace mergen;
int fails = 0;
static void check(bool ok, const QString &what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", qPrintable(what)); if (!ok) fails++;
}
// An independent reader, so the claim is not checked by the code that made it.
static QString textOf(const QString &path, int page) {
    QProcess p;
    p.start(QStringLiteral("pdftotext"),
            {QStringLiteral("-f"), QString::number(page), QStringLiteral("-l"),
             QString::number(page), path, QStringLiteral("-")});
    p.waitForFinished(5000);
    return QString::fromUtf8(p.readAllStandardOutput()).simplified();
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    std::printf("\n--- the text is actually gone, not covered ---\n");
    QFile::remove(QStringLiteral("out.pdf"));
    Redact::Result r = Redact::run(QStringLiteral("test.pdf"), QStringLiteral("out.pdf"), 0,
                                   QRectF(60, 650, 460, 25),
                                   QStringLiteral("searchable haystack needle"));
    check(r.ok, QStringLiteral("redaction reported success (%1)").arg(r.error));
    const QString before = textOf(QStringLiteral("test.pdf"), 1);
    const QString after = textOf(QStringLiteral("out.pdf"), 1);
    std::printf("      before: %s\n      after : %s\n", qPrintable(before), qPrintable(after));
    check(before.contains(QStringLiteral("haystack")), "the phrase was there to begin with");
    check(!after.contains(QStringLiteral("haystack")),
          "pdftotext cannot find it afterwards - it is out of the file, not hidden");
    check(after.contains(QStringLiteral("MERGEN test page 1")),
          "the rest of the page survived");

    std::printf("\n--- other pages are untouched ---\n");
    check(textOf(QStringLiteral("out.pdf"), 2).contains(QStringLiteral("haystack")),
          "page 2 still has its text");
    check(textOf(QStringLiteral("out.pdf"), 3).contains(QStringLiteral("haystack")),
          "page 3 still has its text");

    std::printf("\n--- refusals ---\n");
    r = Redact::run(QStringLiteral("test.pdf"), QStringLiteral("test.pdf"), 0,
                    QRectF(60, 650, 460, 25), QStringLiteral("haystack"));
    check(!r.ok, QStringLiteral("writing over the source is refused: %1").arg(r.error));

    QFile::remove(QStringLiteral("empty.pdf"));
    r = Redact::run(QStringLiteral("test.pdf"), QStringLiteral("empty.pdf"), 0,
                    QRectF(10, 100, 40, 20), QStringLiteral("nothing here"));
    check(!r.ok, QStringLiteral("an empty area is refused: %1").arg(r.error));
    check(!QFile::exists(QStringLiteral("empty.pdf")), "and no file is left behind");

    r = Redact::run(QStringLiteral("test.pdf"), QStringLiteral("nope.pdf"), 99,
                    QRectF(60, 650, 460, 25), QStringLiteral("haystack"));
    check(!r.ok, QStringLiteral("a page that does not exist is refused: %1").arg(r.error));

    std::printf("\n--- the verification backstop actually bites ---\n");
    // Ask it to remove an area that holds one line, but claim the text of a
    // DIFFERENT line that will still be present. The check must catch that and
    // throw the file away rather than call it redacted.
    QFile::remove(QStringLiteral("lie.pdf"));
    r = Redact::run(QStringLiteral("test.pdf"), QStringLiteral("lie.pdf"), 0,
                    QRectF(60, 650, 460, 25), QStringLiteral("MERGEN test page 1"));
    std::printf("      -> ok=%d  \"%s\"\n", int(r.ok), qPrintable(r.error));
    check(!r.ok, "it refused, because the text it was told to remove is still findable");
    check(!QFile::exists(QStringLiteral("lie.pdf")),
          "and the file it had written was discarded, not handed over");

    std::printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", fails);
    return fails;
}
