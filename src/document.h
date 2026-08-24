// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QImage>
#include <QSizeF>
#include <QString>

#include <memory>

#include <poppler-qt6.h>

namespace mergen {

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

    bool isOpen() const { return m_doc != nullptr; }
    QString path() const { return m_path; }

    /// Non-empty only when the file was read through the elevation helper.
    /// The search worker reuses these bytes rather than reopening the path.
    QByteArray data() const { return m_data; }

    int pageCount() const;

    /// Page size in points (1/72 inch), before rotation is applied.
    QSizeF pageSize(int index) const;

    QImage renderPage(int index, double scale, Poppler::Page::Rotation rotation) const;

private:
    LoadStatus adopt(std::unique_ptr<Poppler::Document> doc, const QByteArray &password);
    void applyRenderHints();

    std::unique_ptr<Poppler::Document> m_doc;
    QString m_path;
    QByteArray m_data;
};

} // namespace mergen
