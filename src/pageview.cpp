// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "pageview.h"
#include "document.h"

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollBar>

#include <cmath>

namespace mergen {

PageView::PageView(QWidget *parent) : QAbstractScrollArea(parent) {
    viewport()->setAutoFillBackground(false);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
}

void PageView::setDocument(Document *doc) {
    m_doc = doc;
    m_message.clear();
    m_cache.clear();
    m_zoom = 1.0;
    m_rotation = Poppler::Page::Rotate0;
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

    if (!m_doc || !m_doc->isOpen()) {
        verticalScrollBar()->setRange(0, 0);
        horizontalScrollBar()->setRange(0, 0);
        return;
    }

    // M1 lays out the first page only; the full column arrives with M2.
    const int laidOut = qMin(1, m_doc->pageCount());

    int y = kPageGap;
    int widest = 0;
    for (int i = 0; i < laidOut; ++i) {
        QSizeF points = m_doc->pageSize(i);
        if (m_rotation == Poppler::Page::Rotate90 || m_rotation == Poppler::Page::Rotate270) {
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
    verticalScrollBar()->setRange(0, qMax(0, m_content.height() - view.height()));
    verticalScrollBar()->setPageStep(view.height());
    verticalScrollBar()->setSingleStep(qMax(1, view.height() / 20));
    horizontalScrollBar()->setRange(0, qMax(0, m_content.width() - view.width()));
    horizontalScrollBar()->setPageStep(view.width());
    horizontalScrollBar()->setSingleStep(qMax(1, view.width() / 20));
}

const QImage &PageView::cachedPage(int index) {
    auto it = m_cache.find(index);
    if (it == m_cache.end()) {
        it = m_cache.insert(index, m_doc->renderPage(index, m_zoom, m_rotation));
    }
    return it.value();
}

void PageView::paintEvent(QPaintEvent *event) {
    QPainter painter(viewport());
    painter.fillRect(event->rect(), palette().base());

    if (m_layout.isEmpty()) {
        paintEmptyState(painter);
        return;
    }

    // Content origin, accounting for scroll and for a column narrower than the
    // viewport (which is then centred rather than left-aligned).
    const QSize view = viewport()->size();
    const int originX = m_content.width() <= view.width()
                            ? (view.width() - m_content.width()) / 2
                            : -horizontalScrollBar()->value();
    const int originY = -verticalScrollBar()->value();

    for (int i = 0; i < m_layout.size(); ++i) {
        const QRect target = m_layout.at(i).translated(originX, originY);
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

} // namespace mergen
