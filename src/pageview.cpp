// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "pageview.h"
#include "document.h"

#include <QKeyEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QWheelEvent>

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
    m_zoom = 1.0;
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

    relayout();
    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(0);
    viewport()->update();
}

void PageView::setMessage(const QString &text) {
    m_message = text;
    viewport()->update();
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
    }
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
    relayout();
}

void PageView::scrollContentsBy(int dx, int dy) {
    Q_UNUSED(dx);
    Q_UNUSED(dy);
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
    default:
        break;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void PageView::wheelEvent(QWheelEvent *event) {
    QAbstractScrollArea::wheelEvent(event);
}

} // namespace mergen
