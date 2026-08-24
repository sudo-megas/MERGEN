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

/// How the zoom factor is chosen. Fit modes are recomputed on every resize;
/// Fixed is whatever the user last dialled in.
enum class ZoomMode {
    Fixed,
    FitWidth,
    FitPage,
};

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

    double zoom() const { return m_zoom; }
    ZoomMode zoomMode() const { return m_zoomMode; }

    void setZoom(double factor);
    void zoomIn();
    void zoomOut();
    void resetZoom();
    void setFitWidth();
    void setFitPage();
    void rotateClockwise();
    void rotateCounterClockwise();

    static constexpr double kMinZoom = 0.10;
    static constexpr double kMaxZoom = 10.0;
    static constexpr double kZoomStep = 0.10;

Q_SIGNALS:
    void zoomChanged(double factor);

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

    /// A point of the document expressed independently of zoom: which page,
    /// and where inside it, so that a zoom change can put it back under the
    /// viewport centre.
    struct Anchor {
        int page = -1;
        double fx = 0.0;
        double fy = 0.0;
    };
    Anchor captureAnchor() const;
    void restoreAnchor(const Anchor &anchor);

    /// Zoom implied by the current fit mode, the viewport size and the given
    /// orientation. Returns the existing zoom when the mode is Fixed. Takes
    /// the rotation explicitly so a pending rotation can be measured before it
    /// is committed.
    double zoomForFitMode(Poppler::Page::Rotation rotation) const;
    /// Applies a new zoom and/or rotation, clearing the cache and holding the
    /// anchor point steady. Emits zoomChanged when the factor moves.
    void applyScale(double factor, Poppler::Page::Rotation rotation);

    /// Gap between consecutive pages, and the margin around the column, in
    /// device pixels at any zoom.
    static constexpr int kPageGap = 12;

    /// Pages rendered beyond the viewport on each side, per MX.md Â§8.
    static constexpr int kRenderMargin = 1;

    Document *m_doc = nullptr;
    QString m_message;

    double m_zoom = 1.0;
    ZoomMode m_zoomMode = ZoomMode::FitWidth;
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
