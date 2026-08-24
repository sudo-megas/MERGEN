// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "document.h"

#include <QFileInfo>
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
}

int Document::pageCount() const {
    return m_doc ? m_doc->numPages() : 0;
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
