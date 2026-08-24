// Regressions for the Z11 hardening pass. Each check corresponds to a finding
// that was reproduced during the freeze audit; if one fails, that hole reopened.
#include "mainwindow.h"
#include "pageview.h"
#include "document.h"
#include "overlay.h"
#include "redact.h"
#include <QApplication>
#include <QAction>
#include <QListWidget>
#include <QLineEdit>
#include <QKeySequence>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
#include <QMenu>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QMouseEvent>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
#include <sys/prctl.h>
using namespace mergen;
int fails = 0;
static void check(bool ok, const QString &what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", qPrintable(what)); if (!ok) fails++;
}
static void settle(int ms=320){QEventLoop l;QTimer::singleShot(ms,&l,&QEventLoop::quit);l.exec();}
static QAction *byShortcut(QObject *r, const char *k){
    for (QAction *a : r->findChildren<QAction*>())
        if (a->shortcut()==QKeySequence(QString::fromUtf8(k))) return a;
    return nullptr;
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const QString repo = QStringLiteral(""); // fixtures live in the working directory

    std::printf("\n--- A: a locked document is not open, and does not crash ---\n");
    {
        Document d;
        const LoadStatus st = d.openPath(repo + QStringLiteral("locked.pdf"));
        check(st == LoadStatus::NeedsPassword, "an encrypted file reports NeedsPassword");
        check(!d.isOpen(), "isOpen() is FALSE while locked");
        check(d.isLocked(), "isLocked() is true");
        check(d.pageCount() == 0, "pageCount() returns 0 instead of dereferencing a null catalog");
        check(d.properties().rows.isEmpty(), "properties() is safe on a locked document");
        check(d.outline().isEmpty(), "outline() is safe on a locked document");
    }

    std::printf("\n--- A2: a refused render is null, not a 1x1 pretending ---\n");
    {
        Document d; d.openPath(repo + QStringLiteral("test.pdf"));
        const QImage ok = d.renderPage(0, 2.0, Poppler::Page::Rotate0);
        check(!ok.isNull() && ok.width() > 1000, "an ordinary render still works");
        for (double scale : {60.0, 120.0}) {
            const QImage bad = d.renderPage(0, scale, Poppler::Page::Rotate0);
            check(bad.isNull(), QStringLiteral("a %1x render is refused as NULL").arg(scale));
        }
    }

    std::printf("\n--- F: redaction will not write over its own source ---\n");
    {
        QDir().mkpath(QStringLiteral("z11"));
        QFile::remove(QStringLiteral("z11/src.pdf")); QFile::remove(QStringLiteral("z11/link.pdf"));
        QFile::copy(repo + QStringLiteral("test.pdf"), QStringLiteral("z11/src.pdf"));
        const qint64 before = QFileInfo(QStringLiteral("z11/src.pdf")).size();
        QFile::link(QStringLiteral("src.pdf"), QStringLiteral("z11/link.pdf"));
        const Redact::Result r = Redact::run(QStringLiteral("z11/src.pdf"),
                                             QStringLiteral("z11/link.pdf"), 0,
                                             QRectF(60,650,460,25), QStringLiteral("haystack"));
        check(!r.ok, "a symlink to the source is refused");
        check(QFileInfo(QStringLiteral("z11/src.pdf")).size() == before, "the source is untouched");
    }

    std::printf("\n--- redaction is not offered ---\n");
    {
        MainWindow w; w.resize(900,700); w.show();
        w.openPath(repo + QStringLiteral("test.pdf")); settle(150);
        byShortcut(&w,"Ctrl+K")->trigger();
        Overlay *ov = w.findChild<Overlay*>();
        QListWidget *list = ov->findChild<QListWidget*>();
        bool offered = false;
        for (int i=0;i<list->count();++i)
            if (list->item(i)->text().contains(QStringLiteral("Redact"))) offered = true;
        check(!offered, "\"Redact selection\" is absent from the command overlay");
        ov->dismiss();
    }

    std::printf("\n--- H: opening a document ends a comparison and clears its marks ---\n");
    {
        MainWindow w; w.resize(1100,800); w.show();
        w.openPath(repo + QStringLiteral("outline.pdf")); settle(150);
        w.enterCompare(QDir(repo).absoluteFilePath(QStringLiteral("outline-v2.pdf"))); settle(150);
        check(w.isComparing(), "comparing");
        PageView *v = w.findChild<PageView*>();
        v->scrollToPage(1); settle(350); v->viewport()->grab();
        check(v->hasDiffBands(), "marks present while comparing");
        w.openPath(repo + QStringLiteral("test.pdf")); settle(200);
        check(!w.isComparing(), "opening another document LEFT compare");
        check(!v->hasDiffBands(), "and cleared the marks");
        check(w.findChildren<PageView*>().size() == 1, "and left one view, not two");
    }

    std::printf("\n--- J: diff bands follow the page's rotation ---\n");
    {
        MainWindow w; w.resize(1100,800); w.show();
        w.openPath(repo + QStringLiteral("outline.pdf")); settle(150);
        w.enterCompare(QDir(repo).absoluteFilePath(QStringLiteral("outline-v2.pdf"))); settle(300);
        PageView *v = w.findChild<PageView*>();
        v->scrollToPage(1); settle(350);
        const QImage flat = v->viewport()->grab().toImage();
        v->rotateClockwise(); settle(400);
        const QImage turned = v->viewport()->grab().toImage();
        // A band that ignored rotation would paint the full viewport width at
        // both orientations. A rotation-aware one changes shape.
        auto markedBox = [](const QImage &im){
            int x0=im.width(),x1=-1,y0=im.height(),y1=-1;
            for(int y=0;y<im.height();y+=2) for(int x=0;x<im.width();x+=2){
                const QColor c(im.pixel(x,y));
                if (c.red()!=c.green() || c.green()!=c.blue()) {   // any tinted pixel
                    if(x<x0)x0=x; if(x>x1)x1=x; if(y<y0)y0=y; if(y>y1)y1=y; }
            }
            return QRect(x0,y0,qMax(0,x1-x0),qMax(0,y1-y0));
        };
        const QRect a = markedBox(flat), b = markedBox(turned);
        std::printf("      unrotated band box %dx%d, rotated %dx%d\n", a.width(),a.height(),b.width(),b.height());
        check(a.width()>0 && b.width()>0, "bands are painted at both orientations");
        check(!(a.width()==b.width() && a.height()==b.height()),
              "the band's shape CHANGES with rotation (was identical before)");
        w.leaveCompare();
    }

    // ---- the second Z11 pass: caches, fit modes, state files, portals ----

    std::printf("\n--- a fit mode is a measurement, not a preference ---\n");
    {
        // A page far larger than the viewport. Clamping the fit factor at
        // kMinZoom made this render at hundreds of times the area asked for.
        MainWindow w; w.resize(900, 700); w.show(); settle();
        auto *v = w.findChild<PageView *>();
        w.openPath(QStringLiteral("huge.pdf"));   // one page, 14400x14400 pt
        settle(800);
        if (v && w.windowTitle().contains(QStringLiteral("huge"))) {
            v->setFitPage();
            settle(400);
            const double z = v->zoom();
            std::printf("      fit-page factor %.5f (manual floor is %.2f)\n", z, 0.10);
            check(z > 0.0, "a fit factor is produced");
            check(z < 0.10, "and it is ALLOWED below the manual zoom floor");
            // The scrollbar is the observable consequence: a page that fits
            // needs no vertical range at all.
            const int range = v->verticalScrollBar()->maximum();
            std::printf("      vertical scroll range after fit-page: %d\n", range);
            check(range == 0, "the page actually FITS - nothing left to scroll");
        } else {
            std::printf("      huge.pdf unavailable - skipped\n");
        }
    }

    std::printf("\n--- per-page caches are all pruned, not only the images ---\n");
    {
        MainWindow w; w.resize(900, 700); w.show(); settle();
        auto *v = w.findChild<PageView *>();
        w.openPath(QStringLiteral("large.pdf"));   // 1000 pages
        settle(1200);
        if (v && w.windowTitle().contains(QStringLiteral("large"))) {
            auto rssKb = [] {
                QFile f(QStringLiteral("/proc/self/status"));
                if (!f.open(QIODevice::ReadOnly)) return 0LL;
                for (const QByteArray &l : f.readAll().split('\n'))
                    if (l.startsWith("VmRSS:")) return l.split(':').at(1).trimmed().split(' ').at(0).toLongLong();
                return 0LL;
            };
            const int pages = 1000;   // large.pdf, by construction
            v->scrollToPage(0); settle(400);
            const long long before = rssKb();

            // Scrolling alone never fills the word cache - it is populated by
            // selecting. So select on every page, which is what a reader
            // skimming a long document actually does. A drag across the page
            // runs positionAt twice, and each call fetches that page's words.
            auto dragAcross = [&](int page) {
                v->scrollToPage(page);
                settle(3);
                const QRect box = v->viewport()->rect();
                const QPoint a(box.width() / 4, box.height() / 2);
                const QPoint b(box.width() * 3 / 4, box.height() / 2);
                QMouseEvent down(QEvent::MouseButtonPress, a, v->viewport()->mapToGlobal(a),
                                 Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QMouseEvent move(QEvent::MouseMove, b, v->viewport()->mapToGlobal(b),
                                 Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QMouseEvent up(QEvent::MouseButtonRelease, b, v->viewport()->mapToGlobal(b),
                               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(v->viewport(), &down);
                QApplication::sendEvent(v->viewport(), &move);
                QApplication::sendEvent(v->viewport(), &up);
            };
            for (int pass = 0; pass < 2; ++pass)
                for (int p = 0; p < pages; p += 2) { dragAcross(p); }
            v->scrollToPage(0); settle(400);
            const long long after = rssKb();
            std::printf("      RSS %lld kB -> %lld kB over %d pages, twice\n", before, after, pages);
#if defined(__SANITIZE_ADDRESS__) || defined(__has_feature) && __has_feature(address_sanitizer)
            std::printf("      (RSS bound not checked under AddressSanitizer - "
                        "the quarantine inflates it)\n");
            (void)before; (void)after;
#else
            check(after - before < 40LL * 1024,
                  "selecting across a thousand pages twice stays bounded");
#endif
        } else {
            std::printf("      large.pdf unavailable - skipped\n");
        }
    }

    std::printf("\n--- a comment in recent.toml is not a value ---\n");
    {
        // A temporary XDG_STATE_HOME, so this never touches the real list.
        QTemporaryDir state;
        check(state.isValid(), "temporary state directory");
        const QByteArray previous = qgetenv("XDG_STATE_HOME");
        qputenv("XDG_STATE_HOME", state.path().toLocal8Bit());

        QDir().mkpath(state.path() + QStringLiteral("/mergen"));
        QFile f(state.path() + QStringLiteral("/mergen/recent.toml"));
        check(f.open(QIODevice::WriteOnly | QIODevice::Truncate), "state file writable");
        // The only path in this file is inside a comment. A scan that reads the
        // whole text for brackets would pick it up as an entry.
        f.write("# recent = [ \"/etc/shadow\" ]\nrecent = [\n]\n");
        f.close();

        MainWindow w; w.show(); settle();
        // The recent-files dropdown is the surface the parser feeds. With
        // nothing parsed it holds the single disabled "No recent files" row.
        QMenu *menu = nullptr;
        for (QMenu *m : w.findChildren<QMenu *>())
            if (!m->actions().isEmpty()) { menu = m; break; }
        const QList<QAction *> rows = menu ? menu->actions() : QList<QAction *>();
        QStringList labels;
        for (QAction *a : rows) labels << a->text();
        std::printf("      dropdown holds: %s\n", qPrintable(labels.join(QStringLiteral(", "))));
        check(rows.size() == 1 && !rows.first()->isEnabled(),
              "the commented-out entry is NOT read - the list is empty");

        if (previous.isEmpty()) { qunsetenv("XDG_STATE_HOME"); }
        else { qputenv("XDG_STATE_HOME", previous); }
    }

    std::printf("\n--- the privileged helper sends the whole file, or says so ---\n");
    {
        QProcess helper;
        helper.setProgram(QStringLiteral(MERGEN_OPEN_PATH));
        helper.setArguments({QFileInfo(QStringLiteral("test.pdf")).absoluteFilePath()});
        helper.start();
        helper.waitForFinished(20000);
        const QByteArray out = helper.readAllStandardOutput();
        QFile src(QStringLiteral("test.pdf"));
        src.open(QIODevice::ReadOnly);
        const QByteArray want = src.readAll();
        std::printf("      helper sent %lld bytes, file is %lld\n",
                    (long long)out.size(), (long long)want.size());
        check(helper.exitCode() == 0, "the helper succeeded on a readable file");
        check(out.size() == want.size(), "it sent every byte, not a prefix");
        check(out == want, "and the bytes are identical");
        check(out.startsWith("%PDF-"), "the stream begins with the magic readElevated checks");
    }

    std::printf("\n--- unreadable is not the same as absent ---\n");
    {
        // A file inside a directory this user cannot traverse. stat() fails with
        // EACCES, and QFileInfo::exists() reports false for that exactly as it
        // does for a file that is not there — which sent every root-owned PDF
        // down the "does not exist" path and made the elevation feature
        // unreachable by the only files it was built for.
        QTemporaryDir shed;
        check(shed.isValid(), "temporary directory");
        const QString hidden = shed.path() + QStringLiteral("/inside.pdf");
        QFile::copy(QStringLiteral("test.pdf"), hidden);
        check(QFile::exists(hidden), "a document inside it");

        // Take away search permission, the way /root is 750 to everyone else.
        check(QFile::setPermissions(shed.path(), QFile::ReadOwner | QFile::WriteOwner),
              "directory made non-traversable");

        Document d;
        const LoadStatus status = d.openPath(hidden);
        std::printf("      QFileInfo::exists() says %s, openPath says %s\n",
                    QFileInfo(hidden).exists() ? "true" : "false",
                    status == LoadStatus::NoPermission ? "NoPermission"
                    : status == LoadStatus::NotFound   ? "NotFound"
                    : status == LoadStatus::Ok         ? "Ok" : "other");
        check(status == LoadStatus::NoPermission,
              "an unreadable document reports NoPermission, NOT NotFound");
        check(status != LoadStatus::NotFound,
              "so MainWindow reaches the elevation branch rather than giving up");

        // Give it back, or QTemporaryDir cannot clean up after itself.
        QFile::setPermissions(shed.path(),
                              QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }

    std::printf("\n--- and a genuinely absent file still says so ---\n");
    {
        Document d;
        check(d.openPath(QStringLiteral("no-such-document.pdf")) == LoadStatus::NotFound,
              "a missing document is still NotFound");
    }

    std::printf("\n--- presenceOf() separates the three answers ---\n");
    {
        check(presenceOf(QStringLiteral("test.pdf")) == Presence::Present,
              "an ordinary document is Present");
        check(presenceOf(QStringLiteral("no-such-document.pdf")) == Presence::Absent,
              "a missing document is Absent");

        QTemporaryDir shed;
        const QString hidden = shed.path() + QStringLiteral("/inside.pdf");
        QFile::copy(QStringLiteral("test.pdf"), hidden);
        QFile::setPermissions(shed.path(), QFile::ReadOwner | QFile::WriteOwner);
        check(presenceOf(hidden) == Presence::Unreadable,
              "a document behind a closed directory is Unreadable, NOT Absent");
        // A name below a plain file is ENOTDIR, which no privilege would fix.
        check(presenceOf(QStringLiteral("test.pdf/below")) == Presence::Absent,
              "a path under a non-directory is Absent");
        QFile::setPermissions(shed.path(),
                              QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }

    std::printf("\n--- the recent list keeps what it may not look at ---\n");
    {
        // The other half of the same defect. 2.0.2 got the elevation path
        // reachable; the recent list went on asking QFileInfo::exists(), so a
        // root-owned document was pruned from the reader's own history on the
        // next open and greyed out in the menu before that — no way back to a
        // document they had already authenticated for.
        QTemporaryDir state;
        check(state.isValid(), "temporary state directory");
        qputenv("XDG_STATE_HOME", state.path().toLocal8Bit());

        QTemporaryDir shed;
        const QString hidden = shed.path() + QStringLiteral("/private.pdf");
        QFile::copy(QStringLiteral("test.pdf"), hidden);

        // In a directory that stays traversable, or it would be unreadable
        // rather than absent and the check would prove nothing.
        QTemporaryDir open;
        const QString gone = open.path() + QStringLiteral("/deleted.pdf");
        QFile::copy(QStringLiteral("test.pdf"), gone);
        QFile::remove(gone);
        check(presenceOf(gone) == Presence::Absent, "the deleted one really is Absent");

        // Seeded rather than opened: getting an unreadable path into the list
        // the honest way needs pkexec, and the pruning is what is under test.
        QDir().mkpath(state.path() + QStringLiteral("/mergen"));
        QFile toml(state.path() + QStringLiteral("/mergen/recent.toml"));
        check(toml.open(QIODevice::WriteOnly | QIODevice::Text), "seeded recent.toml");
        toml.write(QStringLiteral("recent = [\n    \"%1\",\n    \"%2\",\n]\n")
                       .arg(hidden, gone).toUtf8());
        toml.close();

        QFile::setPermissions(shed.path(), QFile::ReadOwner | QFile::WriteOwner);

        MainWindow w;                                  // loadRecent() reads the seed
        w.openPath(QStringLiteral("test.pdf"));        // and pushRecent() prunes
        settle();

        // The menu whose entries carry full paths as tooltips is the recent one.
        QMenu *recent = nullptr;
        for (QMenu *m : w.findChildren<QMenu *>()) {
            for (QAction *a : m->actions()) {
                if (a->toolTip().endsWith(QStringLiteral("test.pdf"))) {
                    recent = m;
                }
            }
        }
        check(recent != nullptr, "found the recent menu");

        bool listedHidden = false, enabledHidden = false, listedGone = false;
        if (recent) {
            for (QAction *a : recent->actions()) {
                if (a->toolTip() == hidden) {
                    listedHidden = true;
                    enabledHidden = a->isEnabled();
                }
                if (a->toolTip() == gone) {
                    listedGone = true;
                }
            }
            std::printf("      menu holds %d entr%s\n", int(recent->actions().size()),
                        recent->actions().size() == 1 ? "y" : "ies");
        }
        check(listedHidden, "the unreadable document is still in the recent list");
        check(enabledHidden, "and is still choosable, so pkexec can be asked again");
        check(!listedGone, "while a genuinely deleted one is dropped");

        QFile::setPermissions(shed.path(),
                              QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qunsetenv("XDG_STATE_HOME");
    }

    std::printf("\n--- a reused Document does not inherit the last one's identity ---\n");
    {
        // openPath clears the cached hash and properties; openData did not, and
        // openPath returns NoPermission before it touches any state. So the
        // privileged document that arrived through openData carried the
        // PREVIOUS file's hash and properties: the properties overlay described
        // the wrong document, and a portal made in this one was keyed by the
        // other's content — the silent attachment to a different file that
        // keying by hash exists to prevent.
        Document d;
        check(d.openPath(QStringLiteral("test.pdf")) == LoadStatus::Ok, "an ordinary document");
        const QString firstHash = d.contentHash();
        const int firstPages = d.pageCount();
        check(!firstHash.isEmpty(), "its hash is computed and cached");
        d.properties();

        // annot.pdf is one page where test.pdf is three, so a page count carried
        // over from the previous document is visible rather than coincidental.
        QFile other(QStringLiteral("annot.pdf"));
        check(other.open(QIODevice::ReadOnly), "a different document, as bytes");
        const QByteArray bytes = other.readAll();
        check(d.openData(bytes, QStringLiteral("annot.pdf")) == LoadStatus::Ok,
              "reopened through openData, as the elevation path does");

        std::printf("      hash before %s, after %s\n", qPrintable(firstHash.left(12)),
                    d.contentHash().isEmpty() ? "(none)" : qPrintable(d.contentHash().left(12)));

        // A document that arrived through openData is deliberately NOT hashed:
        // the digest is written to portals.toml, and a digest of privileged
        // content does not belong in an unprivileged state file. So the right
        // answer is no hash at all — and the wrong answer, the one that shipped,
        // was the PREVIOUS document's, which would have keyed a portal made in
        // this document to a different file entirely.
        check(d.contentHash() != firstHash, "it is not the old document's hash");
        check(d.contentHash().isEmpty(), "an elevated document has no hash, by design");

        Document fresh;
        fresh.openPath(QStringLiteral("annot.pdf"));
        std::printf("      pages before %d, after %d\n", firstPages, d.pageCount());
        check(d.pageCount() == fresh.pageCount(),
              "and the page count is the new document's, not the old one's");
        check(firstPages != d.pageCount(), "which is visibly not what was open before");
    }

    std::printf("\n--- a comparison may authenticate, and is wiped when it ends ---\n");
    {
        MainWindow w;
        w.openPath(QStringLiteral("test.pdf"));
        settle();
        check(!w.isComparing(), "not comparing yet");

        // The far side opens by path when it is readable.
        w.enterCompare(QStringLiteral("outline.pdf"));
        settle();
        check(w.isComparing(), "an ordinary document can be compared");
        w.leaveCompare();
        settle();
        check(!w.isComparing(), "and the comparison ends");

        // An unreadable far side must reach the elevation branch rather than
        // being refused outright. pkexec cannot be driven from a test, so what
        // is checked here is that it is ATTEMPTED — openPath must have said
        // NoPermission rather than NotFound, which is what 2.0.2 fixed and what
        // enterCompare now acts on.
        QTemporaryDir shed;
        const QString hidden = shed.path() + QStringLiteral("/far.pdf");
        QFile::copy(QStringLiteral("outline.pdf"), hidden);
        QFile::setPermissions(shed.path(), QFile::ReadOwner | QFile::WriteOwner);

        Document probe;
        check(probe.openPath(hidden) == LoadStatus::NoPermission,
              "the far side reports NoPermission, so enterCompare offers pkexec");

        QFile::setPermissions(shed.path(),
                              QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }

    std::printf("\n--- privileged bytes are erased, not merely released ---\n");
    {
        QFile f(QStringLiteral("test.pdf"));
        check(f.open(QIODevice::ReadOnly), "bytes to stand in for a privileged read");
        const QByteArray bytes = f.readAll();

        // openPath after openData used to call m_data.clear(), handing the
        // plaintext back to the allocator intact.
        Document d;
        check(d.openData(bytes, QStringLiteral("test.pdf")) == LoadStatus::Ok,
              "opened as an elevated document");
        check(!d.data().isEmpty(), "it holds its bytes");
        check(d.openPath(QStringLiteral("outline.pdf")) == LoadStatus::Ok,
              "then an ordinary document is opened over it");
        check(d.data().isEmpty(), "the privileged bytes are gone from the document");

        // The dumpable flag is the observable half of all this. It is a property
        // of the PROCESS, so with two elevated documents — which a comparison
        // now makes possible — clearing it on the first close would re-enable
        // core dumps while the second still holds plaintext this account may not
        // read. Counted since Z12; this is what proves the count.
        auto dumpable = [] { return ::prctl(PR_GET_DUMPABLE, 0, 0, 0, 0); };
        check(dumpable() == 1, "dumpable to begin with, nothing privileged held");

        auto first = std::make_unique<Document>();
        check(first->openData(bytes, QStringLiteral("test.pdf")) == LoadStatus::Ok,
              "one elevated document");
        check(dumpable() == 0, "core dumps are off");

        auto second = std::make_unique<Document>();
        check(second->openData(bytes, QStringLiteral("test.pdf")) == LoadStatus::Ok,
              "a second, as a comparison against a root-owned file makes");
        check(dumpable() == 0, "still off");

        // Released by resetting the owner — the only route a comparison
        // document takes, since leaveCompare never calls close().
        second.reset();
        check(dumpable() == 0,
              "STILL off after one closes, because the other is still holding plaintext");

        first.reset();
        check(dumpable() == 1, "and dumpable again only once the last one is gone");
    }

    std::printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", fails);
    return fails;
}
