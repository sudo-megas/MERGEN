// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "document.h"

#include <QFileInfo>

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
    if (doc->isLocked() && !doc->unlock(password, password)) {
        // Keep the handle: the caller prompts and calls unlock through a retry.
        m_doc = std::move(doc);
        return LoadStatus::NeedsPassword;
    }

    doc->setRenderHint(Poppler::Document::Antialiasing, true);
    doc->setRenderHint(Poppler::Document::TextAntialiasing, true);
    doc->setRenderHint(Poppler::Document::TextSlightHinting, true);

    m_doc = std::move(doc);
    return LoadStatus::Ok;
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

} // namespace mergen
