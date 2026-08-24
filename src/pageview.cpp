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

#include <QApplication>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QScrollBar>

#include <algorithm>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QWheelEvent>

#include <cmath>
#include <limits>

namespace mergen {
namespace {

/// Inverts a page's lightness, holding its hue and saturation.
///
/// Straight RGB inversion is one call and would have been cheaper, but it turns
/// every photograph into a negative and every red chart cyan, which is why so
/// many tools offering a dark PDF mode are unusable on anything but plain text.
///
/// In HSL the chroma C = (1 - |2L-1|) * S is unchanged by L -> 1-L, so only the
/// offset m = L - C/2 moves, and the whole transform collapses to adding
/// 255 - (max + min) to every channel. It is its own inverse, so toggling twice
/// returns the original image exactly.
QImage invertLightness(const QImage &in) {
    QImage out = in.convertToFormat(QImage::Format_RGB32);
    const int height = out.height();
    const int width = out.width();
    for (int y = 0; y < height; ++y) {
        auto *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const QRgb pixel = line[x];
            const int r = qRed(pixel);
            const int g = qGreen(pixel);
            const int b = qBlue(pixel);
            const int shift = 255 - (std::max({r, g, b}) + std::min({r, g, b}));
            line[x] = qRgb(std::clamp(r + shift, 0, 255), std::clamp(g + shift, 0, 255),
                           std::clamp(b + shift, 0, 255));
        }
    }
    return out;
}

/// Whether transitions are drawn at all.
///
/// MZ.md asked for animation to be skipped when the platform reports that the
/// reader wants reduced motion. Qt exposes no such signal on this one. The
/// nearest thing, QApplication::isEffectEnabled(Qt::UI_General), reads false
/// under the wayland, minimal, offscreen and vnc plugins alike whenever no
/// desktop environment has supplied UiEffects — which is precisely a bare Niri
/// session, MERGEN's own target. Gating on it would have meant shipping motion
/// that never once ran for the reader it was built for, and "the platform
/// declined" cannot be told apart from "nobody was asked".
///
/// So motion is on. The one place it is decided is here, so a ruling on an
/// opt-out has somewhere to land — see MZ.md §13.
bool motionWanted() {
    return true;
}

/// Long enough to be followed by the eye, short enough not to be waited on.
/// Fluent, Material and Apple all land in this range for a transition whose job
/// is to keep the reader's place rather than to decorate.
constexpr int kScrollEaseMs = 200;

/// How long the pointer must be held on a link before its target is peeked.
/// Long enough that a click through a link is never mistaken for a peek, short
/// enough that holding does not feel like waiting.
constexpr int kPeekHoldMs = 300;

/// How far the pointer may drift while held before the peek is called off.
constexpr int kPeekSlopPx = 6;

} // namespace

PageView::PageView(QWidget *parent) : QAbstractScrollArea(parent) {
    viewport()->setAutoFillBackground(false);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // Mouse tracking so a held press can be called off when the pointer drifts
    // off the link it started on.
    viewport()->setMouseTracking(true);

    m_peekTimer = new QTimer(this);
    m_peekTimer->setSingleShot(true);
    connect(m_peekTimer, &QTimer::timeout, this, [this] {
        if (m_peekPage >= 0) {
            m_peeking = true;
            Q_EMIT linkPeekRequested(m_peekPage);
        }
    });
}

void PageView::setDocument(Document *doc) {
    m_doc = doc;
    m_message.clear();
    m_cache.clear();
    m_words.clear();
    m_links.clear();
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

void PageView::animateScrollTo(QScrollBar *bar, int value) {
    const int target = qBound(bar->minimum(), value, bar->maximum());
    QPointer<QPropertyAnimation> &slot = bar == verticalScrollBar() ? m_scrollAnimV : m_scrollAnimH;

    if (!motionWanted()) {
        if (slot) {
            slot->stop();
        }
        bar->setValue(target);
        return;
    }

    if (slot) {
        slot->stop();
    } else {
        slot = new QPropertyAnimation(bar, "value", this);
        slot->setEasingCurve(QEasingCurve::OutCubic);
        slot->setDuration(kScrollEaseMs);
    }
    // From where the bar actually is, never from where the last animation was
    // headed: that is what keeps a second jump mid-flight from snapping.
    slot->setStartValue(bar->value());
    slot->setEndValue(target);
    slot->start();
}

void PageView::scrollToPage(int index) {
    if (index < 0 || index >= m_layout.size()) {
        return;
    }
    // Land on the page top rather than centring it: the reader wants to start
    // reading at the top of the page they asked for.
    animateScrollTo(verticalScrollBar(), m_layout.at(index).top() - kPageGap);
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
        // The one transform stage between poppler's decode and the cache. A
        // page rendered inverted and a page rendered plainly are not the same
        // image, so the cache is dropped whenever the mode changes rather than
        // being asked to hold both — see setNightMode.
        QImage page = m_doc->renderPage(index, m_zoom, m_rotation);
        if (m_night && !page.isNull()) {
            page = invertLightness(page);
        }
        it = m_cache.insert(index, page);
    }
    return it.value();
}

QColor PageView::surroundColour() const {
    // Presentation fills the surround with black regardless of palette or mode:
    // anything else is light thrown at whoever is watching.
    if (m_presenting) {
        return Qt::black;
    }
    const QColor base = palette().color(QPalette::Base);
    if (!m_night) {
        return base;
    }
    // The canvas the pages sit on is not chrome, and a night mode that leaves
    // it glowing has not done the one thing it was turned on for. The same
    // lightness inversion is applied to it, so it stays derived from the
    // reader's palette rather than named here.
    const int shift = 255 - (std::max({base.red(), base.green(), base.blue()}) +
                             std::min({base.red(), base.green(), base.blue()}));
    return QColor(std::clamp(base.red() + shift, 0, 255), std::clamp(base.green() + shift, 0, 255),
                  std::clamp(base.blue() + shift, 0, 255));
}

void PageView::stopScrollAnimations() {
    if (m_scrollAnimV) {
        m_scrollAnimV->stop();
    }
    if (m_scrollAnimH) {
        m_scrollAnimH->stop();
    }
}

void PageView::setPresenting(bool on) {
    if (m_presenting == on) {
        return;
    }
    m_presenting = on;
    if (on) {
        setFitPage();
    }
    viewport()->update();
}

void PageView::setNightMode(bool on) {
    if (m_night == on) {
        return;
    }
    m_night = on;
    // Every cached page was rendered for the mode that just ended.
    m_cache.clear();
    viewport()->update();
    Q_EMIT nightModeChanged(m_night);
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
    painter.fillRect(event->rect(), surroundColour());

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

const QVector<PageLink> &PageView::linksOf(int page) {
    auto found = m_links.constFind(page);
    if (found != m_links.constEnd()) {
        return found.value();
    }
    QVector<PageLink> links;
    if (m_doc) {
        links = m_doc->pageLinks(page, m_rotation);
    }
    return *m_links.insert(page, links);
}

const PageLink *PageView::linkAt(const QPoint &viewportPoint) {
    if (!m_doc || m_layout.isEmpty()) {
        return nullptr;
    }
    const QPoint origin = contentOrigin();
    for (int page = 0; page < m_layout.size(); ++page) {
        const QRect box = m_layout.at(page).translated(origin);
        if (!box.contains(viewportPoint)) {
            continue;
        }
        for (const PageLink &link : linksOf(page)) {
            if (fromPageSpace(page, link.area).translated(origin).contains(viewportPoint)) {
                return &link;
            }
        }
        return nullptr;
    }
    return nullptr;
}

void PageView::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && noticeRect().contains(event->position().toPoint())) {
        Q_EMIT noticeClicked();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        if (const PageLink *link = linkAt(event->position().toPoint())) {
            m_peekPage = link->page;
            m_peekOrigin = event->position().toPoint();
            m_peekTimer->start(kPeekHoldMs);
            event->accept();
            return;
        }
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
                const bool newLine = page != previousPage || word.box.top() >= previousBottom;
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
        animateScrollTo(verticalScrollBar(), target.center().y() - viewport()->height() / 3);
        if (m_content.width() > viewport()->width()) {
            animateScrollTo(horizontalScrollBar(), target.center().x() - viewport()->width() / 2);
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
    if (m_peekTimer->isActive() &&
        (event->position().toPoint() - m_peekOrigin).manhattanLength() > kPeekSlopPx) {
        m_peekTimer->stop();
        m_peekPage = -1;
    }
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
    if (m_peekTimer->isActive() || m_peeking) {
        m_peekTimer->stop();
        if (m_peeking) {
            m_peeking = false;
            Q_EMIT linkPeekEnded();
        }
        m_peekPage = -1;
        event->accept();
        return;
    }
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_dragging = false;
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void PageView::paintWatermark(QPainter &painter, const QRect &box) {
    // A sheet with its corner turned, the same gesture the application icon
    // makes. Proportions, not pixels, so it scales with the window.
    const int side = qMin(box.width(), box.height()) / 3;
    if (side < 48) {
        return;
    }
    const QRectF sheet(box.center().x() - side * 0.35, box.center().y() - side * 0.62, side * 0.70,
                       side * 0.92);
    const double curl = side * 0.26;

    QPainterPath page;
    page.moveTo(sheet.topLeft());
    page.lineTo(sheet.right() - curl, sheet.top());
    page.lineTo(sheet.right(), sheet.top() + curl);
    page.lineTo(sheet.bottomRight());
    page.lineTo(sheet.bottomLeft());
    page.closeSubpath();

    QPainterPath fold;
    fold.moveTo(sheet.right() - curl, sheet.top());
    fold.lineTo(sheet.right() - curl, sheet.top() + curl);
    fold.lineTo(sheet.right(), sheet.top() + curl);

    QColor ink = palette().color(QPalette::PlaceholderText);
    ink.setAlpha(58);
    QPen pen(ink);
    pen.setWidthF(qMax(1.5, side * 0.018));
    pen.setJoinStyle(Qt::MiterJoin);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(page);
    painter.drawPath(fold);
    painter.restore();
}

void PageView::paintEmptyState(QPainter &painter) {
    // With no document and nothing to report, the view still says what it is
    // for. One line, always the same, dismissable by using the application —
    // which is what keeps it an empty state rather than onboarding.
    const QString caption =
        m_message.isEmpty() ? tr("Drop a PDF here, or press Ctrl+O") : m_message;
    const QRect box = viewport()->rect().adjusted(32, 32, -32, -32);
    paintWatermark(painter, box);
    painter.setPen(palette().color(QPalette::PlaceholderText));
    // Text sits below the mark rather than over it, so neither is read through
    // the other.
    const int side = qMin(box.width(), box.height()) / 3;
    const QRect line = side < 48 ? box : box.adjusted(0, box.height() / 2 + int(side * 0.42), 0, 0);
    painter.drawText(line, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, caption);
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
        // Presenting moves a page at a time; reading moves a viewport at a time.
        if (m_presenting) {
            scrollToPage(currentPage() + 1);
        } else {
            vbar->setValue(vbar->value() + vbar->pageStep());
        }
        return;
    case Qt::Key_PageUp:
        if (m_presenting) {
            scrollToPage(currentPage() - 1);
        } else {
            vbar->setValue(vbar->value() - vbar->pageStep());
        }
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
    // Any jump still running is aimed at a layout that is about to change.
    stopScrollAnimations();
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
        m_links.clear();
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
