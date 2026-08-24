// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "pageview.h"
#include "document.h"

#include <QKeyEvent>
#include <QClipboard>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QWheelEvent>

#include <cmath>
#include <limits>

namespace mergen {

PageView::PageView(QWidget *parent) : QAbstractScrollArea(parent) {
    viewport()->setAutoFillBackground(false);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
}

void PageView::setDocument(Document *doc) {
    m_doc = doc;
    m_message.clear();
    m_cache.clear();
    m_words.clear();
    m_hits.clear();
    m_currentHit = -1;
    m_selectionAnchor = Position();
    m_selectionCursor = Position();
    m_zoom = 1.0;
    m_zoomMode = ZoomMode::FitWidth;
    m_rotation = Poppler::Page::Rotate0;

    // Page geometry is fixed for the life of the document; ask poppler once.
    m_pageSizes.clear();
    if (m_doc && m_doc->isOpen()) {
        const int count = m_doc->pageCount();
        m_pageSizes.reserve(count);
        for (int i = 0; i < count; ++i) {
            m_pageSizes.append(m_doc->pageSize(i));
        }
    }

    m_zoom = zoomForFitMode(m_rotation);
    relayout();
    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(0);
    m_reportedPage = -1;
    Q_EMIT zoomChanged(m_zoom);
    emitPageIfChanged();
    viewport()->update();
}

void PageView::setMessage(const QString &text) {
    m_message = text;
    viewport()->update();
}

void PageView::setNotice(const QString &text) {
    if (m_notice == text) {
        return;
    }
    m_notice = text;
    viewport()->update();
}

int PageView::currentPage() const {
    return m_layout.isEmpty() ? -1 : captureAnchor().page;
}

void PageView::scrollToPage(int index) {
    if (index < 0 || index >= m_layout.size()) {
        return;
    }
    // Land on the page top rather than centring it: the reader wants to start
    // reading at the top of the page they asked for.
    verticalScrollBar()->setValue(m_layout.at(index).top() - kPageGap);
    emitPageIfChanged();
}

void PageView::emitPageIfChanged() {
    const int page = currentPage();
    if (page != m_reportedPage) {
        m_reportedPage = page;
        Q_EMIT pageChanged(page);
    }
}

void PageView::relayout() {
    m_layout.clear();
    m_content = QSize();

    if (m_pageSizes.isEmpty()) {
        verticalScrollBar()->setRange(0, 0);
        horizontalScrollBar()->setRange(0, 0);
        return;
    }

    const bool quarterTurn =
        m_rotation == Poppler::Page::Rotate90 || m_rotation == Poppler::Page::Rotate270;

    m_layout.reserve(m_pageSizes.size());
    int y = kPageGap;
    int widest = 0;
    for (QSizeF points : m_pageSizes) {
        if (quarterTurn) {
            points.transpose();
        }
        const int w = qMax(1, qRound(points.width() * m_zoom));
        const int h = qMax(1, qRound(points.height() * m_zoom));
        m_layout.append(QRect(0, y, w, h));
        widest = qMax(widest, w);
        y += h + kPageGap;
    }

    m_content = QSize(widest, y);

    // Centre each page horizontally within the content width.
    for (QRect &rect : m_layout) {
        rect.moveLeft((widest - rect.width()) / 2);
    }

    const QSize view = viewport()->size();
    QScrollBar *vbar = verticalScrollBar();
    vbar->setRange(0, qMax(0, m_content.height() - view.height()));
    vbar->setPageStep(view.height());
    // Pixel-based, not page-stepped: one notch moves a line, not a screen.
    vbar->setSingleStep(qBound(20, view.height() / 20, 80));

    QScrollBar *hbar = horizontalScrollBar();
    hbar->setRange(0, qMax(0, m_content.width() - view.width()));
    hbar->setPageStep(view.width());
    hbar->setSingleStep(qBound(20, view.width() / 20, 80));
}

QPoint PageView::contentOrigin() const {
    const QSize view = viewport()->size();
    // A column narrower than the viewport is centred rather than left-aligned.
    const int x = m_content.width() <= view.width() ? (view.width() - m_content.width()) / 2
                                                    : -horizontalScrollBar()->value();
    return QPoint(x, -verticalScrollBar()->value());
}

QPair<int, int> PageView::visibleRange() const {
    if (m_layout.isEmpty()) {
        return {-1, -1};
    }
    const int top = verticalScrollBar()->value();
    const int bottom = top + viewport()->height();

    int first = -1;
    int last = -1;
    for (int i = 0; i < m_layout.size(); ++i) {
        const QRect &r = m_layout.at(i);
        if (r.bottom() < top) {
            continue;
        }
        if (r.top() > bottom) {
            break;
        }
        if (first < 0) {
            first = i;
        }
        last = i;
    }
    if (first < 0) {
        // Scrolled into an inter-page gap past the last page.
        first = last = m_layout.size() - 1;
    }
    return {first, last};
}

const QImage &PageView::cachedPage(int index) {
    auto it = m_cache.find(index);
    if (it == m_cache.end()) {
        it = m_cache.insert(index, m_doc->renderPage(index, m_zoom, m_rotation));
    }
    return it.value();
}

void PageView::dropPagesOutside(int first, int last) {
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        if (it.key() < first || it.key() > last) {
            it = m_cache.erase(it);
        } else {
            ++it;
        }
    }
}

void PageView::paintEvent(QPaintEvent *event) {
    QPainter painter(viewport());
    painter.fillRect(event->rect(), palette().base());

    if (m_layout.isEmpty()) {
        paintEmptyState(painter);
        paintNotice(painter);
        return;
    }

    const auto [firstVisible, lastVisible] = visibleRange();
    const int first = qMax(0, firstVisible - kRenderMargin);
    const int last = qMin(m_layout.size() - 1, lastVisible + kRenderMargin);

    // Bound the cache to the render window so memory tracks the viewport, not
    // the page count.
    dropPagesOutside(first, last);

    const QPoint origin = contentOrigin();
    for (int i = first; i <= last; ++i) {
        const QRect target = m_layout.at(i).translated(origin);
        if (!target.intersects(event->rect())) {
            continue;
        }
        const QImage &image = cachedPage(i);
        if (image.isNull()) {
            continue;
        }
        painter.drawImage(target.topLeft(), image);
        paintSelection(painter, i, origin);
        paintSearchHits(painter, i, origin);
    }

    paintNotice(painter);
}

QRect PageView::noticeRect() const {
    if (m_notice.isEmpty()) {
        return QRect();
    }
    const QFontMetrics fm(font());
    const int w = qMin(viewport()->width() - 2 * kPageGap, fm.horizontalAdvance(m_notice) + 32);
    const int h = fm.height() + 16;
    return QRect((viewport()->width() - w) / 2, kPageGap, w, h);
}

void PageView::paintNotice(QPainter &painter) {
    const QRect bar = noticeRect();
    if (bar.isNull()) {
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette().color(QPalette::Highlight));
    painter.drawRoundedRect(bar, 4, 4);
    painter.setPen(palette().color(QPalette::HighlightedText));
    painter.drawText(bar, Qt::AlignCenter, m_notice);
    painter.restore();
}

void PageView::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && noticeRect().contains(event->position().toPoint())) {
        Q_EMIT noticeClicked();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && !m_layout.isEmpty()) {
        const Position position = positionAt(event->position().toPoint());
        m_selectionAnchor = position;
        m_selectionCursor = position;
        m_dragging = true;
        viewport()->update();
        event->accept();
        return;
    }
    QAbstractScrollArea::mousePressEvent(event);
}


// --- Coordinate mapping ----------------------------------------------------
//
// Poppler returns text boxes and search rects in the *rotated* page space at
// 72 dpi, the same space renderToImage produces its pixels in. So one scale
// factor maps between page points and content pixels at any rotation, and no
// per-rotation special case is needed here.

QPointF PageView::toPageSpace(int page, const QPoint &viewportPoint) const {
    if (page < 0 || page >= m_layout.size() || m_zoom <= 0.0) {
        return QPointF();
    }
    const QRect rect = m_layout.at(page).translated(contentOrigin());
    return QPointF((viewportPoint.x() - rect.left()) / m_zoom,
                   (viewportPoint.y() - rect.top()) / m_zoom);
}

QRect PageView::fromPageSpace(int page, const QRectF &pageRect) const {
    if (page < 0 || page >= m_layout.size()) {
        return QRect();
    }
    const QRect rect = m_layout.at(page);
    return QRectF(rect.left() + pageRect.x() * m_zoom, rect.top() + pageRect.y() * m_zoom,
                  pageRect.width() * m_zoom, pageRect.height() * m_zoom)
        .toAlignedRect();
}

const QVector<Word> &PageView::wordsOf(int page) {
    auto it = m_words.find(page);
    if (it == m_words.end()) {
        it = m_words.insert(page, m_doc ? m_doc->words(page, m_rotation) : QVector<Word>());
    }
    return it.value();
}

PageView::Position PageView::positionAt(const QPoint &viewportPoint) {
    if (m_layout.isEmpty()) {
        return Position();
    }
    const QPoint origin = contentOrigin();

    // Which page: the one under the point, else the nearest vertically, so a
    // drag into the gap between pages still resolves somewhere sensible.
    int page = 0;
    int best = std::numeric_limits<int>::max();
    for (int i = 0; i < m_layout.size(); ++i) {
        const QRect rect = m_layout.at(i).translated(origin);
        const int distance = viewportPoint.y() < rect.top()      ? rect.top() - viewportPoint.y()
                             : viewportPoint.y() > rect.bottom() ? viewportPoint.y() - rect.bottom()
                                                                 : 0;
        if (distance < best) {
            best = distance;
            page = i;
            if (distance == 0) {
                break;
            }
        }
    }

    const QVector<Word> &words = wordsOf(page);
    if (words.isEmpty()) {
        return Position{page, 0};
    }

    const QPointF point = toPageSpace(page, viewportPoint);

    int nearest = 0;
    double nearestDistance = std::numeric_limits<double>::max();
    for (int i = 0; i < words.size(); ++i) {
        const QRectF &box = words.at(i).box;
        if (box.contains(point)) {
            return Position{page, i};
        }
        // Weight vertical distance heavily so that a point to the right of a
        // line picks that line's word rather than one on the line below.
        const double dx = point.x() < box.left()    ? box.left() - point.x()
                          : point.x() > box.right() ? point.x() - box.right()
                                                    : 0.0;
        const double dy = point.y() < box.top()      ? box.top() - point.y()
                          : point.y() > box.bottom() ? point.y() - box.bottom()
                                                     : 0.0;
        const double distance = dx + dy * 4.0;
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = i;
        }
    }
    return Position{page, nearest};
}

// --- Selection -------------------------------------------------------------

void PageView::clearSelection() {
    if (!m_selectionAnchor.isValid() && !m_selectionCursor.isValid()) {
        return;
    }
    m_selectionAnchor = Position();
    m_selectionCursor = Position();
    viewport()->update();
}

QString PageView::selectedText() const {
    if (!m_selectionAnchor.isValid() || !m_selectionCursor.isValid()) {
        return QString();
    }
    Position from = m_selectionAnchor;
    Position to = m_selectionCursor;
    if (to < from) {
        std::swap(from, to);
    }

    auto *self = const_cast<PageView *>(this);
    QString text;
    double previousBottom = -1.0;
    int previousPage = -1;

    for (int page = from.page; page <= to.page && page < m_layout.size(); ++page) {
        const QVector<Word> &words = self->wordsOf(page);
        const int first = page == from.page ? from.word : 0;
        const int last = page == to.page ? to.word : words.size() - 1;
        for (int i = first; i <= last && i < words.size(); ++i) {
            if (i < 0) {
                continue;
            }
            const Word &word = words.at(i);
            if (!text.isEmpty()) {
                // A new page, or a box that starts below the previous line,
                // ends the line.
                const bool newLine =
                    page != previousPage || word.box.top() >= previousBottom;
                text += newLine ? QLatin1Char('\n') : QLatin1Char(' ');
            }
            text += word.text;
            previousBottom = word.box.center().y();
            previousPage = page;
        }
    }
    return text;
}

void PageView::copySelection() {
    const QString text = selectedText();
    if (!text.isEmpty()) {
        QGuiApplication::clipboard()->setText(text);
    }
}

void PageView::paintSelection(QPainter &painter, int page, const QPoint &origin) {
    if (!m_selectionAnchor.isValid() || !m_selectionCursor.isValid()) {
        return;
    }
    Position from = m_selectionAnchor;
    Position to = m_selectionCursor;
    if (to < from) {
        std::swap(from, to);
    }
    if (page < from.page || page > to.page) {
        return;
    }

    const QVector<Word> &words = wordsOf(page);
    const int first = page == from.page ? from.word : 0;
    const int last = page == to.page ? to.word : words.size() - 1;

    QColor colour = palette().color(QPalette::Highlight);
    colour.setAlpha(90);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colour);
    for (int i = qMax(0, first); i <= last && i < words.size(); ++i) {
        painter.drawRect(fromPageSpace(page, words.at(i).box).translated(origin));
    }
}

// --- Search ----------------------------------------------------------------

QColor PageView::searchColor(int alpha) const {
    QColor colour = palette().color(QPalette::Highlight).toHsv();
    const int hue = colour.hue();
    if (hue >= 0) { // -1 for greys, which have no hue to turn
        colour.setHsv((hue + 180) % 360, colour.saturation(), colour.value());
    }
    colour = colour.toRgb();
    colour.setAlpha(alpha);
    return colour;
}

void PageView::addSearchHit(int page, const QRectF &rect) {
    m_hits.append({page, rect});
    if (m_currentHit < 0) {
        m_currentHit = 0;
    }
    Q_EMIT searchHitsChanged(m_hits.size(), m_currentHit);
    viewport()->update();
}

void PageView::clearSearchHits() {
    if (m_hits.isEmpty() && m_currentHit < 0) {
        return;
    }
    m_hits.clear();
    m_currentHit = -1;
    Q_EMIT searchHitsChanged(0, -1);
    viewport()->update();
}

void PageView::goToSearchHit(int index) {
    if (m_hits.isEmpty()) {
        return;
    }
    // Wrap: the reader who presses Enter past the last hit means the first.
    const int count = m_hits.size();
    m_currentHit = ((index % count) + count) % count;

    const auto &[page, rect] = m_hits.at(m_currentHit);
    if (page >= 0 && page < m_layout.size()) {
        const QSizeF size = m_doc ? m_doc->pageSize(page) : QSizeF();
        const QRect target = fromPageSpace(page, Document::rotateRect(rect, size, m_rotation));
        // Put the hit a third of the way down rather than at the very top, so
        // there is context above it.
        verticalScrollBar()->setValue(target.center().y() - viewport()->height() / 3);
        if (m_content.width() > viewport()->width()) {
            horizontalScrollBar()->setValue(target.center().x() - viewport()->width() / 2);
        }
    }
    Q_EMIT searchHitsChanged(m_hits.size(), m_currentHit);
    viewport()->update();
}

void PageView::nextSearchHit() {
    if (!m_hits.isEmpty()) {
        goToSearchHit(m_currentHit + 1);
    }
}

void PageView::previousSearchHit() {
    if (!m_hits.isEmpty()) {
        goToSearchHit(m_currentHit - 1);
    }
}

void PageView::paintSearchHits(QPainter &painter, int page, const QPoint &origin) {
    if (m_hits.isEmpty()) {
        return;
    }
    const QSizeF size = m_doc ? m_doc->pageSize(page) : QSizeF();
    for (int i = 0; i < m_hits.size(); ++i) {
        if (m_hits.at(i).first != page) {
            continue;
        }
        const QRect target =
            fromPageSpace(page, Document::rotateRect(m_hits.at(i).second, size, m_rotation))
                .translated(origin);
        painter.setPen(Qt::NoPen);
        painter.setBrush(searchColor(102));
        painter.drawRect(target);
        if (i == m_currentHit) {
            // The hit being visited is outlined in the same derived colour
            // rather than given a second one.
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(searchColor(255), 2));
            painter.drawRect(target.adjusted(-1, -1, 1, 1));
        }
    }
}

// --- Mouse -----------------------------------------------------------------

void PageView::mouseMoveEvent(QMouseEvent *event) {
    if (!m_dragging) {
        QAbstractScrollArea::mouseMoveEvent(event);
        return;
    }
    const Position position = positionAt(event->position().toPoint());
    if (position.isValid() && !(position == m_selectionCursor)) {
        m_selectionCursor = position;
        viewport()->update();
    }
    event->accept();
}

void PageView::mouseReleaseEvent(QMouseEvent *event) {
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_dragging = false;
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void PageView::paintEmptyState(QPainter &painter) {
    if (m_message.isEmpty()) {
        return;
    }
    painter.setPen(palette().color(QPalette::PlaceholderText));
    const QRect box = viewport()->rect().adjusted(32, 32, -32, -32);
    painter.drawText(box, Qt::AlignCenter | Qt::TextWordWrap, m_message);
}

void PageView::resizeEvent(QResizeEvent *event) {
    QAbstractScrollArea::resizeEvent(event);
    if (m_zoomMode == ZoomMode::Fixed) {
        relayout();
        return;
    }
    // A fit mode is a standing instruction, not a one-shot: recompute it.
    const double refitted = zoomForFitMode(m_rotation);
    if (!qFuzzyCompare(refitted, m_zoom)) {
        const Anchor anchor = captureAnchor();
        m_zoom = refitted;
        m_cache.clear();
        relayout();
        restoreAnchor(anchor);
        Q_EMIT zoomChanged(m_zoom);
    } else {
        relayout();
    }
}

void PageView::scrollContentsBy(int dx, int dy) {
    Q_UNUSED(dx);
    Q_UNUSED(dy);
    emitPageIfChanged();
    viewport()->update();
}

void PageView::keyPressEvent(QKeyEvent *event) {
    QScrollBar *vbar = verticalScrollBar();
    QScrollBar *hbar = horizontalScrollBar();

    switch (event->key()) {
    case Qt::Key_Down:
        vbar->setValue(vbar->value() + vbar->singleStep());
        return;
    case Qt::Key_Up:
        vbar->setValue(vbar->value() - vbar->singleStep());
        return;
    case Qt::Key_PageDown:
        vbar->setValue(vbar->value() + vbar->pageStep());
        return;
    case Qt::Key_PageUp:
        vbar->setValue(vbar->value() - vbar->pageStep());
        return;
    case Qt::Key_Home:
        vbar->setValue(vbar->minimum());
        return;
    case Qt::Key_End:
        vbar->setValue(vbar->maximum());
        return;
    case Qt::Key_Right:
        // Pans only when the column is wider than the viewport.
        hbar->setValue(hbar->value() + hbar->singleStep());
        return;
    case Qt::Key_Left:
        hbar->setValue(hbar->value() - hbar->singleStep());
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (event->modifiers() & Qt::ShiftModifier) {
            previousSearchHit();
        } else {
            nextSearchHit();
        }
        return;
    case Qt::Key_C:
        if (event->modifiers() & Qt::ControlModifier) {
            copySelection();
            return;
        }
        break;
    case Qt::Key_F:
        if (event->modifiers() & Qt::ShiftModifier) {
            setFitPage();
        } else if (event->modifiers() == Qt::NoModifier) {
            setFitWidth();
        }
        return;
    default:
        break;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void PageView::wheelEvent(QWheelEvent *event) {
    if (event->modifiers() & Qt::ControlModifier) {
        const int delta = event->angleDelta().y();
        if (delta > 0) {
            zoomIn();
        } else if (delta < 0) {
            zoomOut();
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

double PageView::zoomForFitMode(Poppler::Page::Rotation rotation) const {
    if (m_zoomMode == ZoomMode::Fixed || m_pageSizes.isEmpty()) {
        return m_zoom;
    }

    const bool quarterTurn =
        rotation == Poppler::Page::Rotate90 || rotation == Poppler::Page::Rotate270;

    // Both fit modes size against the largest page, so the result does not
    // shift as the reader scrolls between differently sized pages.
    double widest = 0.0;
    double tallest = 0.0;
    for (QSizeF points : m_pageSizes) {
        if (quarterTurn) {
            points.transpose();
        }
        widest = qMax(widest, points.width());
        tallest = qMax(tallest, points.height());
    }
    if (widest <= 0.0 || tallest <= 0.0) {
        return m_zoom;
    }

    const QSize view = viewport()->size();
    const double usableWidth = qMax(1, view.width() - 2 * kPageGap);
    double factor = usableWidth / widest;

    if (m_zoomMode == ZoomMode::FitPage) {
        const double usableHeight = qMax(1, view.height() - 2 * kPageGap);
        factor = qMin(factor, usableHeight / tallest);
    }

    return qBound(kMinZoom, factor, kMaxZoom);
}

PageView::Anchor PageView::captureAnchor() const {
    Anchor anchor;
    if (m_layout.isEmpty()) {
        return anchor;
    }

    const int centreY = verticalScrollBar()->value() + viewport()->height() / 2;
    const int centreX = m_content.width() <= viewport()->width()
                            ? m_content.width() / 2
                            : horizontalScrollBar()->value() + viewport()->width() / 2;

    // The page holding the centre, or the nearest one when the centre falls in
    // an inter-page gap.
    int best = 0;
    int bestDistance = std::numeric_limits<int>::max();
    for (int i = 0; i < m_layout.size(); ++i) {
        const QRect &r = m_layout.at(i);
        const int distance = centreY < r.top()      ? r.top() - centreY
                             : centreY > r.bottom() ? centreY - r.bottom()
                                                    : 0;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
            if (distance == 0) {
                break;
            }
        }
    }

    const QRect &page = m_layout.at(best);
    anchor.page = best;
    anchor.fx = page.width() > 0 ? double(centreX - page.left()) / page.width() : 0.0;
    anchor.fy = page.height() > 0 ? double(centreY - page.top()) / page.height() : 0.0;
    return anchor;
}

void PageView::restoreAnchor(const Anchor &anchor) {
    if (anchor.page < 0 || anchor.page >= m_layout.size()) {
        return;
    }
    const QRect &page = m_layout.at(anchor.page);
    const int targetY = page.top() + qRound(anchor.fy * page.height());
    const int targetX = page.left() + qRound(anchor.fx * page.width());
    verticalScrollBar()->setValue(targetY - viewport()->height() / 2);
    horizontalScrollBar()->setValue(targetX - viewport()->width() / 2);
}

void PageView::applyScale(double factor, Poppler::Page::Rotation rotation) {
    const double clamped = qBound(kMinZoom, factor, kMaxZoom);
    if (qFuzzyCompare(clamped, m_zoom) && rotation == m_rotation) {
        return;
    }

    const Anchor anchor = captureAnchor();
    const bool turned = rotation != m_rotation;
    m_zoom = clamped;
    m_rotation = rotation;
    // Every cached image is at the old scale and orientation.
    m_cache.clear();
    if (turned) {
        // Word boxes are in points and so survive a zoom, but not a rotation:
        // both the boxes and their reading order change with it.
        m_words.clear();
        clearSelection();
    }
    relayout();
    restoreAnchor(anchor);
    Q_EMIT zoomChanged(m_zoom);
    emitPageIfChanged();
    viewport()->update();
}

void PageView::setZoom(double factor) {
    m_zoomMode = ZoomMode::Fixed;
    applyScale(factor, m_rotation);
}

void PageView::zoomIn() {
    // Snap onto the 10% grid so a fit zoom of 87% steps to 90%, not 97%.
    const double stepped = std::floor(m_zoom / kZoomStep + 1e-6) * kZoomStep + kZoomStep;
    setZoom(stepped);
}

void PageView::zoomOut() {
    const double stepped = std::ceil(m_zoom / kZoomStep - 1e-6) * kZoomStep - kZoomStep;
    setZoom(stepped);
}

void PageView::resetZoom() {
    setZoom(1.0);
}

void PageView::setFitWidth() {
    m_zoomMode = ZoomMode::FitWidth;
    applyScale(zoomForFitMode(m_rotation), m_rotation);
}

void PageView::setFitPage() {
    m_zoomMode = ZoomMode::FitPage;
    applyScale(zoomForFitMode(m_rotation), m_rotation);
}

void PageView::rotateClockwise() {
    const auto next = Poppler::Page::Rotation((int(m_rotation) + 1) % 4);
    // Rotating changes which dimension a fit mode measures against, so the
    // new orientation is priced in before it is committed.
    applyScale(m_zoomMode == ZoomMode::Fixed ? m_zoom : zoomForFitMode(next), next);
}

void PageView::rotateCounterClockwise() {
    const auto next = Poppler::Page::Rotation((int(m_rotation) + 3) % 4);
    applyScale(m_zoomMode == ZoomMode::Fixed ? m_zoom : zoomForFitMode(next), next);
}

} // namespace mergen
