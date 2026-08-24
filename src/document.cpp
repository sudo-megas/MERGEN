// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "document.h"

#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>

#include <QCryptographicHash>
#include <QScopeGuard>
#include <cstring>

#include <sys/prctl.h>
#include <QDateTime>
#include <poppler-annotation.h>
#include <poppler-link.h>

#include <QFileInfo>
#include <QLocale>
#include <utility>

namespace mergen {

/// Inverts a page's lightness, holding its hue and saturation.
///
/// Straight RGB inversion is one call and would have been cheaper, but it turns
/// every photograph into a negative and every red chart cyan, which is why so
/// many tools offering a dark PDF mode are unusable on anything but plain text.
///
/// In HSL the chroma C = (1 - |2L-1|) * S is unchanged by L -> 1-L, so only the
/// offset m = L - C/2 moves, and the whole transform collapses to adding
/// 255 - (max + min) to every channel. It is its own inverse, so toggling twice
/// returns the original image exactly.
QImage invertLightness(const QImage &in) {
    QImage out = in.convertToFormat(QImage::Format_RGB32);
    const int height = out.height();
    const int width = out.width();
    for (int y = 0; y < height; ++y) {
        auto *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const QRgb pixel = line[x];
            const int r = qRed(pixel);
            const int g = qGreen(pixel);
            const int b = qBlue(pixel);
            const int shift = 255 - (std::max({r, g, b}) + std::min({r, g, b}));
            line[x] = qRgb(std::clamp(r + shift, 0, 255), std::clamp(g + shift, 0, 255),
                           std::clamp(b + shift, 0, 255));
        }
    }
    return out;
}

namespace {
/// Ceiling on a single rendered page, in bytes. Poppler stops honouring
/// allocations near 2^31 and starts returning 1x1 instead of failing, so the
/// limit is drawn just below that where a refusal is still a refusal.
constexpr long long kMaxRenderBytes = 1536LL * 1024 * 1024;

/// presenceOf(), keeping the stat buffer for the one caller that needs the mode
/// bits as well as the verdict. Both go through here so the rule for reading
/// errno exists once and the two cannot drift apart.
Presence statPresence(const QString &path, struct stat *st) {
    if (::stat(path.toLocal8Bit().constData(), st) == 0) {
        return Presence::Present;
    }
    switch (errno) {
    // The name does not resolve, and no privilege would change that: nothing of
    // that name, a non-directory used as one, a symlink that leads nowhere, or a
    // name too long for any filesystem to be holding.
    case ENOENT:
    case ENOTDIR:
    case ELOOP:
    case ENAMETOOLONG:
        return Presence::Absent;
    // EACCES — a directory on the way is closed to this account, which is the
    // ordinary case for /root at mode 750 — and everything else besides. An
    // unexpected errno is a failure to look, not a finding of absence, and is
    // reported as the former: claiming a file is gone on the strength of an EIO
    // is exactly the conflation this type exists to end.
    default:
        return Presence::Unreadable;
    }
}

/// How many Documents currently hold privileged plaintext.
///
/// PR_SET_DUMPABLE is a property of the process, not of a document, and since
/// Z12 a comparison may authenticate too — so there can be two. Counted rather
/// than set and cleared: the second document closing must not re-enable core
/// dumps while the first is still holding a file this account may not read.
/// Main thread only, which is where Documents are made and closed; the search
/// worker keeps its own copy and never touches this.
int g_elevated = 0;

void retainElevated() {
    if (g_elevated++ == 0) {
        // While a privileged document is resident, a core dump would write its
        // plaintext to disk — one authentication becoming a permanent
        // unauthenticated copy. MZ.md §8 says nothing else is persisted.
        ::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    }
}

void releaseElevated() {
    if (g_elevated > 0 && --g_elevated == 0) {
        ::prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
    }
}
} // namespace

Presence presenceOf(const QString &path) {
    struct stat st{};
    return statPresence(path, &st);
}

Document::Document() = default;

/// Not `= default`. A comparison document is released by resetting the pointer
/// that owns it, never through close(), so a defaulted destructor handed its
/// privileged plaintext back to the allocator intact. The main document reaches
/// the same path when the window is destroyed with a document still open.
Document::~Document() {
    wipeData();
}

LoadStatus Document::openPath(const QString &path, const QByteArray &password) {
    const QFileInfo info(path);

    // Not QFileInfo::exists(). It answers stat() and reports false for every
    // failure alike, so a PDF inside a directory this user cannot traverse —
    // /root, mode 750, which is the ordinary case — came back "does not exist"
    // and the elevation path was never reached at all. The one feature built
    // for exactly that file could not be triggered by it.
    //
    // errno tells the two apart: ENOENT means it is not there, EACCES means we
    // are not permitted to look, and only the second is a question pkexec can
    // answer.
    struct stat st{};
    switch (statPresence(path, &st)) {
    case Presence::Absent:
        return LoadStatus::NotFound;
    case Presence::Unreadable:
        return LoadStatus::NoPermission;
    case Presence::Present:
        break;
    }
    if (!S_ISREG(st.st_mode)) {
        return LoadStatus::NotFound;
    }
    // Readable in principle is not readable in fact — a mode-600 file owned by
    // someone else sits in a directory we can traverse, and stat succeeds.
    if (::access(path.toLocal8Bit().constData(), R_OK) != 0) {
        return LoadStatus::NoPermission;
    }

    auto doc = Poppler::Document::load(path);
    if (!doc) {
        return LoadStatus::Invalid;
    }

    const LoadStatus status = adopt(std::move(doc), password);
    if (status == LoadStatus::Ok || status == LoadStatus::NeedsPassword) {
        m_path = info.absoluteFilePath();
        // wipeData, not clear: this object may have been holding a privileged
        // document a moment ago, and clear() drops that plaintext into freed
        // heap without erasing it. Opening any ordinary file afterwards was
        // enough to do it.
        wipeData();
        m_hash.clear();
        m_properties.reset();
    }
    return status;
}

LoadStatus Document::openData(const QByteArray &bytes, const QString &path,
                              const QByteArray &password) {
    if (bytes.isEmpty()) {
        return LoadStatus::Invalid;
    }

    auto doc = Poppler::Document::loadFromData(bytes);
    if (!doc) {
        return LoadStatus::Invalid;
    }

    const LoadStatus status = adopt(std::move(doc), password);
    if (status == LoadStatus::Ok || status == LoadStatus::NeedsPassword) {
        m_path = QFileInfo(path).absoluteFilePath();
        // Both of these were missing, and openPath has always cleared them. A
        // Document is reused for every open, and openPath returns NoPermission
        // before it touches any state — so the privileged document that arrived
        // here inherited the *previous* file's cached hash and properties. The
        // properties overlay described the wrong document, and a portal made in
        // this one was keyed by the other's content, which is precisely the
        // silent attachment to a different file that keying by hash exists to
        // prevent.
        m_hash.clear();
        m_properties.reset();
        // Wipe before taking the new bytes: a second privileged open on this
        // object would otherwise abandon the first document's plaintext
        // unzeroed and count its elevation twice.
        wipeData();
        m_data = bytes;
        retainElevated();
    }
    return status;
}

LoadStatus Document::adopt(std::unique_ptr<Poppler::Document> doc, const QByteArray &password) {
    // Poppler::Document::unlock() reports whether the document is *still*
    // locked, so a true return means the password did not open it. Loading has
    // already tried the empty password, so an empty one here cannot help.
    if (doc->isLocked() && (password.isEmpty() || doc->unlock(password, password))) {
        // Keep the handle: the caller prompts and calls unlock through a retry.
        // isOpen() stays false meanwhile — the catalog is not there yet.
        m_doc = std::move(doc);
        m_locked = true;
        return LoadStatus::NeedsPassword;
    }

    m_doc = std::move(doc);
    m_locked = false;
    applyRenderHints();
    return LoadStatus::Ok;
}

void Document::applyRenderHints() {
    if (!m_doc) {
        return;
    }
    m_doc->setRenderHint(Poppler::Document::Antialiasing, true);
    m_doc->setRenderHint(Poppler::Document::TextAntialiasing, true);
    m_doc->setRenderHint(Poppler::Document::TextSlightHinting, true);
    // Poppler composites annotations into the page by default, so MERGEN has
    // always drawn them — but by default rather than by decision, and a default
    // can move. Reading a PDF faithfully includes reading what is written on
    // it, so the intent is stated here rather than inherited — MZ.md §9.
    m_doc->setRenderHint(Poppler::Document::HideAnnotations, false);
}

bool Document::unlock(const QByteArray &password) {
    if (!m_doc) {
        return false;
    }
    if (!m_doc->isLocked()) {
        m_locked = false;
        return true;
    }
    if (m_doc->unlock(password, password)) {
        return false; // still locked: wrong password
    }
    m_locked = false;
    // unlock() rebuilds poppler's internal document, so the hints set at load
    // time are gone with it.
    applyRenderHints();
    return true;
}

void Document::wipeData() {
    if (m_data.isEmpty()) {
        return;
    }
    // data() detaches first, so this erases the buffer this object owns.
    std::memset(m_data.data(), 0, static_cast<size_t>(m_data.size()));
    m_data.clear();
    // Dumpable again only once no document holds privileged plaintext — this is
    // the last one out, not merely one of them.
    releaseElevated();
}

void Document::close() {
    m_doc.reset();
    m_locked = false;
    m_path.clear();
    wipeData();
    m_hash.clear();
    m_properties.reset();
}

int Document::pageCount() const {
    return isOpen() ? m_doc->numPages() : 0;
}

namespace {

/// Walks poppler's outline tree depth-first into a flat list.
/// A table of contents deep enough to exhaust the stack is not one a reader
/// wrote. Poppler catches cycles; nothing here caught depth, and a document can
/// choose it freely.
constexpr int kMaxOutlineDepth = 64;

void flattenOutline(const QVector<Poppler::OutlineItem> &items, int depth,
                    QVector<OutlineEntry> &out) {
    if (depth > kMaxOutlineDepth) {
        return;
    }
    for (const Poppler::OutlineItem &item : items) {
        if (item.isNull()) {
            continue;
        }
        OutlineEntry entry;
        entry.title = item.name().simplified();
        entry.depth = depth;

        // An entry pointing into another file, or at a URL, is listed and does
        // nothing: MERGEN opens neither.
        if (item.externalFileName().isEmpty() && item.uri().isEmpty()) {
            if (const QSharedPointer<const Poppler::LinkDestination> dest = item.destination()) {
                // poppler counts pages from one here; everything else in MERGEN
                // counts from zero.
                entry.page = dest->pageNumber() - 1;
            }
        }
        out.append(entry);

        if (item.hasChildren()) {
            flattenOutline(item.children(), depth + 1, out);
        }
    }
}

} // namespace

QVector<OutlineEntry> Document::outline() const {
    QVector<OutlineEntry> out;
    if (!isOpen()) {
        return out;
    }
    flattenOutline(m_doc->outline(), 0, out);
    return out;
}

QVector<PageLink> Document::pageLinks(int index, Poppler::Page::Rotation rotation) const {
    QVector<PageLink> out;
    if (!isOpen() || index < 0 || index >= m_doc->numPages()) {
        return out;
    }
    const std::unique_ptr<Poppler::Page> page = m_doc->page(index);
    if (!page) {
        return out;
    }

    const QSizeF size = page->pageSizeF();
    for (const std::unique_ptr<Poppler::Link> &link : page->links()) {
        if (!link || link->linkType() != Poppler::Link::Goto) {
            continue;
        }
        const auto *jump = static_cast<const Poppler::LinkGoto *>(link.get());
        if (jump->isExternal()) {
            continue;
        }
        const int target = jump->destination().pageNumber() - 1;
        if (target < 0 || target >= m_doc->numPages()) {
            continue;
        }

        // linkArea is fractional, and poppler hands back rects whose corners
        // are not always in the order a QRectF expects.
        const QRectF fraction = link->linkArea().normalized();
        const QRectF points(fraction.x() * size.width(), fraction.y() * size.height(),
                            fraction.width() * size.width(), fraction.height() * size.height());

        out.append({rotateRect(points, size, rotation), target});
    }
    return out;
}

QString Document::contentHash() const {
    if (!m_hash.isEmpty() || !m_doc) {
        return m_hash;
    }
    // A document that arrived through the elevation helper is deliberately NOT
    // hashed: that digest is written to portals.toml, and a digest of
    // privileged content does not belong in an unprivileged state file.
    if (!m_data.isEmpty()) {
        return QString();
    }
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (hash.addData(&file)) {
        m_hash = QString::fromLatin1(hash.result().toHex());
    }
    return m_hash;
}

DocumentProperties Document::properties() const {
    DocumentProperties out;
    if (!isOpen()) {
        return out;
    }
    // fonts() scans every page. On a font-heavy document that measured minutes,
    // synchronously, from a keystroke — and again on every repeat because
    // nothing was kept.
    if (m_properties) {
        return *m_properties;
    }

    const auto row = [&out](const QString &name, const QString &value) {
        if (!value.isEmpty()) {
            out.rows.append({name, value});
        }
    };

    row(QStringLiteral("File"), QFileInfo(m_path).fileName());

    // Only the keys the document actually carries, in a fixed order rather than
    // poppler's, so two documents describe themselves the same way.
    for (const auto &key :
         {QStringLiteral("Title"), QStringLiteral("Subject"), QStringLiteral("Author"),
          QStringLiteral("Keywords"), QStringLiteral("Creator"), QStringLiteral("Producer")}) {
        row(key, m_doc->info(key).simplified());
    }

    const QLocale locale;
    for (const auto &pair : {std::pair{QStringLiteral("CreationDate"), QStringLiteral("Created")},
                             std::pair{QStringLiteral("ModDate"), QStringLiteral("Modified")}}) {
        const QDateTime when = m_doc->date(pair.first);
        if (when.isValid()) {
            row(pair.second, locale.toString(when, QLocale::ShortFormat));
        }
    }

    const Poppler::Document::PdfVersion version = m_doc->getPdfVersion();
    row(QStringLiteral("PDF version"),
        QStringLiteral("%1.%2").arg(version.major).arg(version.minor));
    row(QStringLiteral("Pages"), QString::number(m_doc->numPages()));

    if (m_doc->numPages() > 0) {
        const QSizeF size = pageSize(0);
        if (size.isValid()) {
            // Points are the document's own unit; millimetres are the one the
            // reader owns a ruler for.
            row(QStringLiteral("Page size"),
                QStringLiteral("%1 x %2 pt  (%3 x %4 mm)")
                    .arg(QString::number(size.width(), 'f', 0),
                         QString::number(size.height(), 'f', 0),
                         QString::number(size.width() * 25.4 / 72.0, 'f', 0),
                         QString::number(size.height() * 25.4 / 72.0, 'f', 0)));
        }
    }

    row(QStringLiteral("Encrypted"),
        m_doc->isEncrypted() ? QStringLiteral("yes") : QStringLiteral("no"));
    row(QStringLiteral("Linearized"),
        m_doc->isLinearized() ? QStringLiteral("yes") : QStringLiteral("no"));

    // Only what is withheld is worth listing: a document that allows everything
    // says so in one word instead of seven.
    QStringList denied;
    if (!m_doc->okToPrint()) {
        denied << QStringLiteral("print");
    }
    if (!m_doc->okToCopy()) {
        denied << QStringLiteral("copy text");
    }
    if (!m_doc->okToChange()) {
        denied << QStringLiteral("modify");
    }
    if (!m_doc->okToAddNotes()) {
        denied << QStringLiteral("annotate");
    }
    if (!m_doc->okToExtractForAccessibility()) {
        denied << QStringLiteral("extract for accessibility");
    }
    row(QStringLiteral("Restrictions"),
        denied.isEmpty() ? QStringLiteral("none") : denied.join(QStringLiteral(", ")));

    const QList<Poppler::FontInfo> fonts = m_doc->fonts();
    if (!fonts.isEmpty()) {
        int embedded = 0;
        for (const Poppler::FontInfo &font : fonts) {
            if (font.isEmbedded()) {
                ++embedded;
            }
        }
        row(QStringLiteral("Fonts"),
            QStringLiteral("%1 (%2 embedded)")
                .arg(QString::number(fonts.size()), QString::number(embedded)));
    }

    // Markup someone else left. It is drawn either way; this says it is there,
    // so a reader knows the yellow is not part of the document.
    int marks = 0;
    for (int i = 0; i < m_doc->numPages(); ++i) {
        const std::unique_ptr<Poppler::Page> page = m_doc->page(i);
        if (!page) {
            continue;
        }
        for (const std::unique_ptr<Poppler::Annotation> &annot : page->annotations()) {
            // Links are structure, not markup, and are never what a reader
            // means when they ask whether a document has been annotated.
            if (annot && annot->subType() != Poppler::Annotation::ALink &&
                annot->subType() != Poppler::Annotation::AWidget) {
                ++marks;
            }
        }
    }
    if (marks > 0) {
        row(QStringLiteral("Annotations"), QString::number(marks));
    }

    // The part a viewer normally keeps to itself.
    if (!m_doc->scripts().isEmpty()) {
        out.warnings << QStringLiteral(
            "This document carries embedded JavaScript. MERGEN never runs it.");
    }
    if (m_doc->formType() != Poppler::Document::NoForm) {
        out.warnings << QStringLiteral(
            "This document contains form fields. MERGEN does not fill forms.");
    }
    if (m_doc->hasEmbeddedFiles()) {
        out.warnings << QStringLiteral(
            "This document has files attached to it. MERGEN does not open them.");
    }

    m_properties = out;
    return out;
}

QSizeF Document::pageSize(int index) const {
    if (!isOpen() || index < 0 || index >= m_doc->numPages()) {
        return QSizeF();
    }
    const auto page = m_doc->page(index);
    return page ? page->pageSizeF() : QSizeF();
}

QImage Document::renderPage(int index, double scale, Poppler::Page::Rotation rotation) const {
    // Poppler refuses allocations past 2^31 bytes by returning a 1x1 image that
    // is NOT null, so every isNull() check downstream would pass on a failed
    // render — a printed sheet of solid black, a 1x1 cached as the page, two
    // different documents compared as identical. Caught here, once, so those
    // checks start meaning what they say — MZ.md §9.
    if (!isOpen() || index < 0 || index >= m_doc->numPages() || scale <= 0.0) {
        return QImage();
    }
    const auto page = m_doc->page(index);
    if (!page) {
        return QImage();
    }
    const double dpi = 72.0 * scale;

    // What the page should come back as, so a refusal can be told from a render.
    const QSizeF points = page->pageSizeF();
    const bool turned = rotation == Poppler::Page::Rotate90 || rotation == Poppler::Page::Rotate270;
    const double wantWidth = (turned ? points.height() : points.width()) * scale;
    const double wantHeight = (turned ? points.width() : points.height()) * scale;

    // Refuse before poppler has to: past 2^31 bytes it hands back 1x1 rather
    // than failing, and an implausible /MediaBox from the document must not be
    // able to demand gigabytes — MZ.md §9.
    if (wantWidth * wantHeight * 4.0 > double(kMaxRenderBytes)) {
        return QImage();
    }

    QImage image = page->renderToImage(dpi, dpi, -1, -1, -1, -1, rotation);

    // A size wildly unlike the one asked for means poppler declined. One pixel
    // is its tell, but compare properly rather than special-casing 1x1.
    if (!image.isNull() && (image.width() < wantWidth / 2.0 || image.height() < wantHeight / 2.0)) {
        return QImage();
    }
    return image;
}

QVector<Word> Document::words(int index, Poppler::Page::Rotation rotation) const {
    QVector<Word> result;
    if (!isOpen() || index < 0 || index >= m_doc->numPages()) {
        return result;
    }
    const auto page = m_doc->page(index);
    if (!page) {
        return result;
    }
    const auto boxes = page->textList(rotation);
    result.reserve(static_cast<int>(boxes.size()));
    for (const auto &box : boxes) {
        result.append(Word{box->boundingBox(), box->text(), box->hasSpaceAfter()});
    }
    return result;
}

QList<QRectF> Document::search(int index, const QString &needle) const {
    if (!isOpen() || index < 0 || index >= m_doc->numPages() || needle.isEmpty()) {
        return {};
    }
    const auto page = m_doc->page(index);
    if (!page) {
        return {};
    }
    return page->search(needle, Poppler::Page::IgnoreCase);
}

QRectF Document::unrotateRect(const QRectF &rect, const QSizeF &unrotatedSize,
                              Poppler::Page::Rotation rotation) {
    const double w = unrotatedSize.width();
    const double h = unrotatedSize.height();
    switch (rotation) {
    case Poppler::Page::Rotate90:
        // rotateRect90 maps (x,y,rw,rh) -> (h-y-rh, x, rh, rw); read backwards.
        return QRectF(rect.y(), h - rect.x() - rect.width(), rect.height(), rect.width());
    case Poppler::Page::Rotate180:
        return QRectF(w - rect.x() - rect.width(), h - rect.y() - rect.height(), rect.width(),
                      rect.height());
    case Poppler::Page::Rotate270:
        return QRectF(w - rect.y() - rect.height(), rect.x(), rect.height(), rect.width());
    case Poppler::Page::Rotate0:
        break;
    }
    return rect;
}

QRectF Document::rotateRect(const QRectF &rect, const QSizeF &unrotatedSize,
                            Poppler::Page::Rotation rotation) {
    const double w = unrotatedSize.width();
    const double h = unrotatedSize.height();
    switch (rotation) {
    case Poppler::Page::Rotate90:
        return QRectF(h - rect.y() - rect.height(), rect.x(), rect.height(), rect.width());
    case Poppler::Page::Rotate180:
        return QRectF(w - rect.x() - rect.width(), h - rect.y() - rect.height(), rect.width(),
                      rect.height());
    case Poppler::Page::Rotate270:
        return QRectF(rect.y(), w - rect.x() - rect.width(), rect.height(), rect.width());
    case Poppler::Page::Rotate0:
        break;
    }
    return rect;
}

SearchWorker::SearchWorker(QString path, QByteArray data, QString needle, QObject *parent)
    : QObject(parent), m_path(std::move(path)), m_data(std::move(data)),
      m_needle(std::move(needle)) {}

void SearchWorker::run() {
    // The worker's copy of an elevated document is wiped when the pass ends,
    // for the same reason Document::wipeData exists.
    const QScopeGuard wipe([this] {
        if (!m_data.isEmpty()) {
            std::memset(m_data.data(), 0, static_cast<size_t>(m_data.size()));
            m_data.clear();
        }
    });
    // A handle of this thread's own: poppler documents are not shareable.
    auto doc = m_data.isEmpty() ? Poppler::Document::load(m_path)
                                : Poppler::Document::loadFromData(m_data);
    if (!doc || doc->isLocked()) {
        Q_EMIT done(false);
        return;
    }

    const int total = doc->numPages();
    for (int i = 0; i < total; ++i) {
        if (m_cancelled.loadRelaxed()) {
            Q_EMIT done(true);
            return;
        }
        const auto page = doc->page(i);
        if (page) {
            const QList<QRectF> hits = page->search(m_needle, Poppler::Page::IgnoreCase);
            for (const QRectF &rect : hits) {
                Q_EMIT hitFound(i, rect);
            }
        }
        Q_EMIT progress(i + 1, total);
    }
    Q_EMIT done(false);
}

} // namespace mergen
