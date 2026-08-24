// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "document.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <poppler-annotation.h>
#include <poppler-link.h>

#include <QFileInfo>
#include <QLocale>
#include <utility>

namespace mergen {

Document::Document() = default;
Document::~Document() = default;

LoadStatus Document::openPath(const QString &path, const QByteArray &password) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return LoadStatus::NotFound;
    }
    if (!info.isReadable()) {
        return LoadStatus::NoPermission;
    }

    auto doc = Poppler::Document::load(path);
    if (!doc) {
        return LoadStatus::Invalid;
    }

    const LoadStatus status = adopt(std::move(doc), password);
    if (status == LoadStatus::Ok || status == LoadStatus::NeedsPassword) {
        m_path = info.absoluteFilePath();
        m_data.clear();
        m_hash.clear();
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
        m_data = bytes;
    }
    return status;
}

LoadStatus Document::adopt(std::unique_ptr<Poppler::Document> doc, const QByteArray &password) {
    // Poppler::Document::unlock() reports whether the document is *still*
    // locked, so a true return means the password did not open it. Loading has
    // already tried the empty password, so an empty one here cannot help.
    if (doc->isLocked() && (password.isEmpty() || doc->unlock(password, password))) {
        // Keep the handle: the caller prompts and calls unlock through a retry.
        m_doc = std::move(doc);
        return LoadStatus::NeedsPassword;
    }

    m_doc = std::move(doc);
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
        return true;
    }
    if (m_doc->unlock(password, password)) {
        return false; // still locked: wrong password
    }
    // unlock() rebuilds poppler's internal document, so the hints set at load
    // time are gone with it.
    applyRenderHints();
    return true;
}

void Document::close() {
    m_doc.reset();
    m_path.clear();
    m_data.clear();
    m_hash.clear();
}

int Document::pageCount() const {
    return m_doc ? m_doc->numPages() : 0;
}

namespace {

/// Walks poppler's outline tree depth-first into a flat list.
void flattenOutline(const QVector<Poppler::OutlineItem> &items, int depth,
                    QVector<OutlineEntry> &out) {
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
    if (!m_doc) {
        return out;
    }
    flattenOutline(m_doc->outline(), 0, out);
    return out;
}

QVector<PageLink> Document::pageLinks(int index, Poppler::Page::Rotation rotation) const {
    QVector<PageLink> out;
    if (!m_doc || index < 0 || index >= m_doc->numPages()) {
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
    // Bytes already in hand for a document that came through the elevation
    // helper; otherwise read the file once.
    if (!m_data.isEmpty()) {
        m_hash = QString::fromLatin1(
            QCryptographicHash::hash(m_data, QCryptographicHash::Sha256).toHex());
        return m_hash;
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
    if (!m_doc) {
        return out;
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

    return out;
}

QSizeF Document::pageSize(int index) const {
    if (!m_doc || index < 0 || index >= m_doc->numPages()) {
        return QSizeF();
    }
    const auto page = m_doc->page(index);
    return page ? page->pageSizeF() : QSizeF();
}

QImage Document::renderPage(int index, double scale, Poppler::Page::Rotation rotation) const {
    if (!m_doc || index < 0 || index >= m_doc->numPages() || scale <= 0.0) {
        return QImage();
    }
    const auto page = m_doc->page(index);
    if (!page) {
        return QImage();
    }
    const double dpi = 72.0 * scale;
    return page->renderToImage(dpi, dpi, -1, -1, -1, -1, rotation);
}

QVector<Word> Document::words(int index, Poppler::Page::Rotation rotation) const {
    QVector<Word> result;
    if (!m_doc || index < 0 || index >= m_doc->numPages()) {
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
    if (!m_doc || index < 0 || index >= m_doc->numPages() || needle.isEmpty()) {
        return {};
    }
    const auto page = m_doc->page(index);
    if (!page) {
        return {};
    }
    return page->search(needle, Poppler::Page::IgnoreCase);
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
