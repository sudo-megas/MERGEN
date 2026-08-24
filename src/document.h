// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QAtomicInt>
#include <QByteArray>
#include <QImage>
#include <QList>
#include <QObject>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVector>

#include <memory>
#include <optional>

#include <poppler-qt6.h>

namespace mergen {

/// Inverts an image's lightness while leaving hue and saturation alone, so a red
/// chart stays red. Its own inverse: applying it twice returns the original
/// exactly. Shared by the page view and the thumbnail strip, so the colour maths
/// lives in one place — the derivation is with the definition.
QImage invertLightness(const QImage &in);

/// One word of extracted text and where it sits on the page, in points, in the
/// rotated coordinate space that was asked for.
struct Word {
    QRectF box;
    QString text;
    bool spaceAfter = false;
};

/// One labelled row of the properties overlay.
struct Property {
    QString name;
    QString value;
};

/// What a document says about itself, and what it does not advertise.
///
/// The warnings are the point of this being here at all: a PDF can carry
/// JavaScript, form fields and attached files, and nothing in a viewer's normal
/// surface tells the reader so. MERGEN does not run any of it — it has no
/// JavaScript engine and \ref MZ.md §5 bans form filling outright — but a
/// reader deserves to know a page is carrying more than ink.
struct DocumentProperties {
    QVector<Property> rows;
    QStringList warnings;
};

/// One entry of the document's own table of contents, flattened depth-first
/// with its depth kept so the overlay can indent it. A transient list a reader
/// arrows through wants indentation, not a tree to expand — MZ.md \ref 6.
struct OutlineEntry {
    QString title;
    int depth = 0;
    /// Zero-based, or -1 for an entry that points somewhere MERGEN will not
    /// follow: another file, or a URL. Those are shown, and do nothing.
    int page = -1;
};

/// An internal link and where it goes, for hold-to-peek.
struct PageLink {
    /// In points, in the rotated page space, ready for fromPageSpace().
    QRectF area;
    /// Zero-based destination page.
    int page = -1;
};

/// What the filesystem will say about a path, for the callers whose answer
/// decides whether an entry is dropped, greyed out or refused.
///
/// QFileInfo::exists() cannot answer this. It reports stat()'s success and
/// nothing else, so "there is no such file" and "a directory on the way is
/// closed to this account" come back identically false — which is how the
/// elevation path came to be unreachable by the very files it was built for.
enum class Presence {
    /// stat() succeeded. Whether it can be *read* is a separate question, and
    /// one only openPath() is in a position to answer.
    Present,
    /// The name does not resolve to anything. Safe to drop.
    Absent,
    /// It may well be there; this account is not permitted to look. Never to be
    /// reported, or acted on, as an absence.
    Unreadable,
};

/// stat(), with errno kept rather than discarded. Anything short of a definite
/// absence is Unreadable, because "I could not look" must not be passed off as
/// "it is not there" — MZ.md §13.
Presence presenceOf(const QString &path);

/// Why a load attempt did not produce a usable document.
enum class LoadStatus {
    Ok,
    NotFound,
    NoPermission,
    NeedsPassword,
    Invalid,
};

/// Thin wrapper over Poppler::Document. No abstraction layer, no generator
/// indirection — just the handful of calls the view and the toolbar need.
class Document {
public:
    Document();
    ~Document();

    Document(const Document &) = delete;
    Document &operator=(const Document &) = delete;

    /// Open a file by path. The caller checks for NoPermission and may retry
    /// through the elevation helper.
    LoadStatus openPath(const QString &path, const QByteArray &password = QByteArray());

    /// Open a document already held in memory, as delivered by the elevation
    /// helper. `path` is kept for the window title and the recent list.
    LoadStatus openData(const QByteArray &bytes, const QString &path,
                        const QByteArray &password = QByteArray());

    /// Retry a NeedsPassword document without rereading it from disk. The
    /// caller wipes its copy of the password afterwards; this class keeps none.
    bool unlock(const QByteArray &password);

    void close();

    /// A document that is loaded but still encrypted is NOT open: poppler
    /// leaves its catalog null until it is decrypted, so every accessor would
    /// dereference nothing. adopt() deliberately keeps the handle so unlock()
    /// can retry, which is why the locked state is tracked rather than
    /// inferred from the pointer — MZ.md §9.
    bool isOpen() const { return m_doc != nullptr && !m_locked; }

    /// Loaded, but still waiting for a password.
    bool isLocked() const { return m_doc != nullptr && m_locked; }
    QString path() const { return m_path; }

    /// Non-empty only when the file was read through the elevation helper.
    /// The search worker reuses these bytes rather than reopening the path.
    QByteArray data() const { return m_data; }

    int pageCount() const;

    /// A hash of the document's bytes, computed once and kept. Portals are
    /// keyed by it rather than by path, so one survives its document being
    /// moved and never silently attaches to a different file that happens to
    /// take the old name — MZ.md \ref 8.
    QString contentHash() const;

    /// Metadata, permissions and the security warnings described on
    /// \ref DocumentProperties. Gathered on demand, never at open time: it
    /// walks the font table, which is work no reader asked for until they ask.
    DocumentProperties properties() const;

    /// The document's table of contents, flattened. Empty when it has none,
    /// which many documents genuinely do.
    QVector<OutlineEntry> outline() const;

    /// Internal links on one page, in the orientation asked for. Only links
    /// that go to another page of this same document: an external file or a
    /// URL is not something MERGEN opens — MZ.md \ref 5.
    QVector<PageLink> pageLinks(int index, Poppler::Page::Rotation rotation) const;

    /// Page size in points (1/72 inch), before rotation is applied.
    QSizeF pageSize(int index) const;

    QImage renderPage(int index, double scale, Poppler::Page::Rotation rotation) const;

    /// Words of one page in reading order for the given orientation.
    QVector<Word> words(int index, Poppler::Page::Rotation rotation) const;

    /// Hits on one page, in the unrotated page space. Callers rotate the rects
    /// for display, so a rotation mid-search does not invalidate the results.
    QList<QRectF> search(int index, const QString &needle) const;

    /// Maps a rect from the unrotated page space into the rotated one. The
    /// page size given is the unrotated size.
    static QRectF rotateRect(const QRectF &rect, const QSizeF &unrotatedSize,
                             Poppler::Page::Rotation rotation);

    /// The inverse of \ref rotateRect: takes a rect in the rotated space back
    /// to the unrotated one. Redaction needs it, because what the reader
    /// selected on screen has to be found in the document's own coordinates.
    static QRectF unrotateRect(const QRectF &rect, const QSizeF &unrotatedSize,
                               Poppler::Page::Rotation rotation);

private:
    LoadStatus adopt(std::unique_ptr<Poppler::Document> doc, const QByteArray &password);
    /// Overwrites the elevated document's bytes before releasing them.
    /// QByteArray::clear() frees without erasing, and MX.md §5 takes exactly
    /// this care over the password — the document it unlocks deserves the same.
    void wipeData();
    void applyRenderHints();

    std::unique_ptr<Poppler::Document> m_doc;
    QString m_path;
    QByteArray m_data;
    mutable QString m_hash;
    /// properties() walks the whole font table; the answer cannot change while
    /// the document is open, so it is computed once.
    mutable std::optional<DocumentProperties> m_properties;
    bool m_locked = false;
};

/// Runs a search pass over its own document handle, because poppler's document
/// objects are not safe to share between threads. Lives on a worker thread and
/// reports hits as it finds them, so a hit early in a long document is usable
/// long before the pass ends.
class SearchWorker : public QObject {
    Q_OBJECT

public:
    /// `data` is non-empty for documents that were read through the elevation
    /// helper; the worker reuses those bytes rather than reopening a path it
    /// has no permission to read.
    SearchWorker(QString path, QByteArray data, QString needle, QObject *parent = nullptr);

    /// Safe to call from another thread while run() is in progress.
    void cancel() { m_cancelled.storeRelaxed(1); }

public Q_SLOTS:
    void run();

Q_SIGNALS:
    void hitFound(int page, const QRectF &rect);
    void progress(int page, int total);
    void done(bool cancelled);

private:
    QString m_path;
    QByteArray m_data;
    mutable QString m_hash;
    /// properties() walks the whole font table; the answer cannot change while
    /// the document is open, so it is computed once.
    mutable std::optional<DocumentProperties> m_properties;
    bool m_locked = false;
    QString m_needle;
    QAtomicInt m_cancelled{0};
};

} // namespace mergen
