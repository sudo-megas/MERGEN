// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QAbstractScrollArea>
#include <QHash>
#include <QImage>
#include <QList>
#include <QPair>
#include <QPoint>
#include <QRect>
#include <QSizeF>
#include <QString>
#include <QVector>

#include <poppler-qt6.h>

namespace mergen {

class Document;
struct Word;

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

    /// The page under the viewport centre, zero-based. -1 with no document.
    int currentPage() const;
    void scrollToPage(int index);

    /// A one-line clickable bar drawn at the top of the page area. Empty text
    /// removes it. Used for the on-disk-change reload prompt, so that MERGEN
    /// never interrupts with a dialog.
    void setNotice(const QString &text);

    // --- Selection ---------------------------------------------------------

    /// The selected words joined with spaces, and a newline at each line end.
    QString selectedText() const;
    void copySelection();
    void clearSelection();

    // --- Search ------------------------------------------------------------

    /// Hits arrive one at a time while the worker walks the document, so they
    /// are appended rather than set in one go. Rects are in the unrotated page
    /// space and are rotated for painting.
    void addSearchHit(int page, const QRectF &rect);
    void clearSearchHits();
    int searchHitCount() const { return m_hits.size(); }
    int currentSearchHit() const { return m_currentHit; }
    void goToSearchHit(int index);
    void nextSearchHit();
    void previousSearchHit();

    static constexpr double kMinZoom = 0.10;
    static constexpr double kMaxZoom = 10.0;
    static constexpr double kZoomStep = 0.10;

Q_SIGNALS:
    void zoomChanged(double factor);
    void pageChanged(int index);
    void noticeClicked();
    void searchHitsChanged(int count, int current);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
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
    void emitPageIfChanged();
    /// Notice bar geometry in viewport coordinates; null when no notice is set.
    QRect noticeRect() const;
    void paintNotice(QPainter &painter);

    /// A word of the document, addressed as page plus index into that page's
    /// word list. Ordered lexicographically, which is reading order.
    struct Position {
        int page = -1;
        int word = -1;
        bool isValid() const { return page >= 0 && word >= 0; }
        bool operator<(const Position &o) const {
            return page != o.page ? page < o.page : word < o.word;
        }
        bool operator==(const Position &o) const { return page == o.page && word == o.word; }
    };

    const QVector<Word> &wordsOf(int page);
    /// The word nearest a viewport point, for anchoring and extending a drag.
    Position positionAt(const QPoint &viewportPoint);
    /// Page-space point, in points, for a viewport point on the given page.
    QPointF toPageSpace(int page, const QPoint &viewportPoint) const;
    /// Content-space rect for a rect given in the rotated page space.
    QRect fromPageSpace(int page, const QRectF &pageRect) const;
    void paintSelection(QPainter &painter, int page, const QPoint &origin);
    void paintSearchHits(QPainter &painter, int page, const QPoint &origin);
    /// Derived from QPalette::Highlight at runtime: hue turned through 180
    /// degrees so a hit always contrasts with both the selection and the page.
    QColor searchColor(int alpha) const;

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
    QString m_notice;
    int m_reportedPage = -1;

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

    /// Word boxes per page. Unlike the render cache these do not depend on
    /// zoom, so they survive a zoom change and are dropped only on rotation.
    QHash<int, QVector<Word>> m_words;

    Position m_selectionAnchor;
    Position m_selectionCursor;
    bool m_dragging = false;

    /// (page, rect) with the rect in the unrotated page space.
    QList<QPair<int, QRectF>> m_hits;
    int m_currentHit = -1;
};

} // namespace mergen
