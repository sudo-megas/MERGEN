// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QAbstractScrollArea>
#include <QHash>
#include <QImage>
#include <QPair>
#include <QPoint>
#include <QRect>
#include <QSizeF>
#include <QString>
#include <QVector>

#include <poppler-qt6.h>

namespace mergen {

class Document;

/// The scrolling page column. Owns layout, culling, painting and the render
/// cache; draws the empty state and error text itself rather than raising a
/// dialog.
class PageView : public QAbstractScrollArea {
    Q_OBJECT

public:
    explicit PageView(QWidget *parent = nullptr);

    /// Pass nullptr to return to the empty state.
    void setDocument(Document *doc);

    /// Centred text drawn over the empty page area. Cleared by setDocument.
    void setMessage(const QString &text);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    void relayout();
    void paintEmptyState(QPainter &painter);
    const QImage &cachedPage(int index);
    /// Inclusive page range intersecting the viewport, ignoring the render
    /// margin. Returns {-1, -1} when nothing is laid out.
    QPair<int, int> visibleRange() const;
    /// Top-left of the content area in viewport coordinates.
    QPoint contentOrigin() const;
    void dropPagesOutside(int first, int last);

    /// Gap between consecutive pages, and the margin around the column, in
    /// device pixels at any zoom.
    static constexpr int kPageGap = 12;

    /// Pages rendered beyond the viewport on each side, per MX.md Â§8.
    static constexpr int kRenderMargin = 1;

    Document *m_doc = nullptr;
    QString m_message;

    double m_zoom = 1.0;
    Poppler::Page::Rotation m_rotation = Poppler::Page::Rotate0;

    /// Page sizes in points, cached once per document so that relayout on
    /// every resize does not reconstruct a Poppler::Page per page.
    QVector<QSizeF> m_pageSizes;

    /// Page rectangles in content coordinates, one per page.
    QVector<QRect> m_layout;
    QSize m_content;
    QHash<int, QImage> m_cache;
};

} // namespace mergen
