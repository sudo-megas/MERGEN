// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QAbstractScrollArea>
#include <QPointer>
#include <QTimer>

#include <functional>
#include <QHash>
#include <QImage>
#include <QList>
#include <QPair>
#include <QPoint>
#include <QRect>
#include <QSizeF>
#include <QString>
#include <QVector>

#include "document.h"

#include <poppler-qt6.h>

class QScrollBar;
class QPropertyAnimation;

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
    Poppler::Page::Rotation rotation() const { return m_rotation; }
    ZoomMode zoomMode() const { return m_zoomMode; }

    void setZoom(double factor);
    void zoomIn();
    void zoomOut();
    void resetZoom();
    void setFitWidth();
    void setFitPage();
    void rotateClockwise();
    void rotateCounterClockwise();

    /// Night mode inverts the page's lightness and leaves its hue and
    /// saturation alone, so a red chart stays red — MZ.md \ref 9. Session
    /// scoped: nothing about it is written to disk.
    bool isNightMode() const { return m_night; }
    void setNightMode(bool on);

    /// Presentation mode: black surround, and PgUp/PgDn move exactly one page
    /// rather than one viewport. The window chrome is the window's business.
    bool isPresenting() const { return m_presenting; }
    void setPresenting(bool on);

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

    /// The page the selection sits on and its bounding box in the document's
    /// own unrotated space, or {-1, {}} when nothing is selected. Redaction
    /// works in that space, not in the one on screen.
    QPair<int, QRectF> selectionBounds() const;
    void copySelection();
    void clearSelection();

    // --- Search ------------------------------------------------------------

    /// Hits arrive one at a time while the worker walks the document, so they
    /// are appended rather than set in one go. Rects are in the unrotated page
    /// space and are rotated for painting.
    /// The internal link under a viewport point, or nullptr. Cached per page
    /// alongside the words, and dropped on rotation for the same reason.
    const PageLink *linkAt(const QPoint &viewportPoint);

    /// Bands of a page, as fractions of its height, where a comparison found a
    /// difference. Fractions rather than pixels so the marks survive a zoom.
    void setDiffBands(int page, const QVector<QPair<double, double>> &bands);

    /// Where the marks for a page come from, asked for the first time that page
    /// is painted. Comparing used to render every page of both documents before
    /// showing anything — measured at 9.1 s on a thousand-page pair, on the
    /// thread that draws, with no progress and no way to stop it.
    using DiffProvider = std::function<QVector<QPair<double, double>>(int page)>;
    void setDiffProvider(DiffProvider provider);
    void clearDiffBands();
    /// Whether any difference is actually marked. Not the same as "a
    /// comparison is running" — pages found identical cache an empty result,
    /// and MainWindow::isComparing answers the other question.
    bool hasDiffBands() const {
        for (const auto &bands : m_diffBands) {
            if (!bands.isEmpty()) {
                return true;
            }
        }
        return false;
    }

    void addSearchHit(int page, const QRectF &rect);
    void clearSearchHits();
    int searchHitCount() const { return m_hits.size(); }
    int currentSearchHit() const { return m_currentHit; }
    void goToSearchHit(int index);
    void nextSearchHit();
    void previousSearchHit();

    static constexpr double kMinZoom = 0.10;
    /// How far a fit mode may shrink. Far below kMinZoom, because a page can be
    /// arbitrarily large and fitting it is not a preference the reader set.
    static constexpr double kMinFitZoom = 0.001;
    static constexpr double kMaxZoom = 10.0;
    static constexpr double kZoomStep = 0.10;

Q_SIGNALS:
    void zoomChanged(double factor);
    void pageChanged(int index);
    void noticeClicked();
    void searchHitsChanged(int count, int current);
    void nightModeChanged(bool on);
    /// The reader has held the pointer down on an internal link long enough to
    /// mean it. Carries the destination page, zero-based.
    void linkPeekRequested(int page);
    /// They let go.
    void linkPeekEnded();

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
    /// The application icon's curled-page motif, drawn faintly behind the empty
    /// state. The icon is real illustration work that otherwise stops at the
    /// desktop entry — MZ.md \ref 6.
    void paintWatermark(QPainter &painter, const QRect &box);
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
    const QVector<PageLink> &linksOf(int page);

    /// Moves a scrollbar to \a value, eased, or immediately when the platform
    /// says UI effects are off. Restarting mid-flight picks up from where the
    /// bar actually is, never from where the last animation was headed, which
    /// is what stops a second jump from snapping — MZ.md \ref 9.
    void animateScrollTo(QScrollBar *bar, int value);
    /// Stops any jump in flight. A relayout aims it at coordinates that have
    /// moved, so it is cut rather than allowed to land somewhere wrong.
    void stopScrollAnimations();
    /// The word nearest a viewport point, for anchoring and extending a drag.
    Position positionAt(const QPoint &viewportPoint);
    /// Page-space point, in points, for a viewport point on the given page.
    QPointF toPageSpace(int page, const QPoint &viewportPoint) const;
    /// Content-space rect for a rect given in the rotated page space.
    QRect fromPageSpace(int page, const QRectF &pageRect) const;
    void paintSelection(QPainter &painter, int page, const QPoint &origin);
    void paintSearchHits(QPainter &painter, int page, const QPoint &origin);
    void paintDiffBands(QPainter &painter, int page, const QPoint &origin);
    /// Derived from QPalette::Highlight at runtime: hue turned through 180
    /// degrees so a hit always contrasts with both the selection and the page.
    QColor searchColor(int alpha) const;
    /// The colour behind the pages. QPalette::Base normally; its lightness
    /// inverted in night mode, since the canvas is not chrome.
    QColor surroundColour() const;

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

    bool m_night = false;
    bool m_presenting = false;
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

    /// Link areas per page, on the same terms as the words: points, current
    /// orientation, so they survive a zoom and are dropped on a rotation.
    QHash<int, QVector<PageLink>> m_links;

    /// Hold-to-peek. A press on a link starts the timer instead of a
    /// selection; the peek is only asked for once the reader has held it long
    /// enough to have meant it, and is ended by the release.
    QPointer<QPropertyAnimation> m_scrollAnimV;
    QPointer<QPropertyAnimation> m_scrollAnimH;

    QTimer *m_peekTimer = nullptr;
    int m_peekPage = -1;
    QPoint m_peekOrigin;
    bool m_peeking = false;

    Position m_selectionAnchor;
    Position m_selectionCursor;
    bool m_dragging = false;

    QHash<int, QVector<QPair<double, double>>> m_diffBands;
    DiffProvider m_diffProvider;

    /// (page, rect) with the rect in the unrotated page space.
    QList<QPair<int, QRectF>> m_hits;
    int m_currentHit = -1;
};

} // namespace mergen
