// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "thumbstrip.h"

#include "document.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollBar>

namespace mergen {

ThumbnailStrip::ThumbnailStrip(QWidget *parent) : QAbstractScrollArea(parent) {
    setFrameShape(QFrame::NoFrame);
    horizontalScrollBar()->setEnabled(false);
    horizontalScrollBar()->setRange(0, 0);
    viewport()->setAutoFillBackground(false);
    setAccessibleName(tr("Page previews"));
}

double ThumbnailStrip::renderRatio() const {
    const double ratio = viewport()->devicePixelRatioF();
    return ratio > 0.0 ? ratio : 1.0;
}

void ThumbnailStrip::setDocument(Document *doc) {
    m_doc = doc;
    m_current = doc && doc->isOpen() ? 0 : -1;
    m_cache.clear();
    m_cacheWidth = 0;
    verticalScrollBar()->setValue(0);
    relayout();
    viewport()->update();
}

void ThumbnailStrip::setNightMode(bool on) {
    if (m_night == on) {
        return;
    }
    m_night = on;
    // An inverted preview and a plain one are not the same image, so the cache
    // is dropped rather than asked to hold both.
    m_cache.clear();
    viewport()->update();
}

void ThumbnailStrip::setCurrentPage(int page) {
    if (page == m_current) {
        return;
    }
    m_current = page;

    // Follow the reader. Only when the page has actually left the strip —
    // scrolling it to the same place on every page change would fight anyone
    // browsing the previews by hand.
    if (page >= 0 && page < m_rows.size()) {
        const QRect row = m_rows.at(page);
        const int top = verticalScrollBar()->value();
        const int bottom = top + viewport()->height();
        if (row.top() < top || row.bottom() > bottom) {
            verticalScrollBar()->setValue(row.center().y() - viewport()->height() / 2);
        }
    }
    viewport()->update();
}

QRect ThumbnailStrip::paperRect(int page) const {
    return page >= 0 && page < m_rows.size() ? m_rows.at(page) : QRect();
}

void ThumbnailStrip::relayout() {
    m_rows.clear();
    m_contentHeight = 0;

    if (!m_doc || !m_doc->isOpen()) {
        verticalScrollBar()->setRange(0, 0);
        return;
    }

    const int count = m_doc->pageCount();
    const int paperWidth = qMax(1, viewport()->width() - 2 * kMargin);

    m_rows.reserve(count);
    int y = kMargin;
    for (int i = 0; i < count; ++i) {
        const QSizeF size = m_doc->pageSize(i);
        // A page with no usable size still gets a row, so page numbers keep
        // lining up with the document.
        const double aspect =
            size.isValid() && size.width() > 0 ? size.height() / size.width() : 1.414;
        const int paperHeight = qMax(1, qRound(paperWidth * aspect));
        m_rows.append(QRect(kMargin, y, paperWidth, paperHeight));
        y += paperHeight + kLabelHeight + kMargin;
    }
    m_contentHeight = y;

    QScrollBar *bar = verticalScrollBar();
    bar->setRange(0, qMax(0, m_contentHeight - viewport()->height()));
    bar->setPageStep(viewport()->height());
    bar->setSingleStep(qBound(20, viewport()->height() / 12, 90));
}

void ThumbnailStrip::resizeEvent(QResizeEvent *event) {
    QAbstractScrollArea::resizeEvent(event);
    if (event->oldSize().width() != event->size().width()) {
        // Every cached preview was rendered for the old width.
        m_cache.clear();
    }
    relayout();
}

const QImage &ThumbnailStrip::thumbnailFor(int page) {
    auto it = m_cache.find(page);
    if (it != m_cache.end()) {
        return it.value();
    }

    const QRect row = paperRect(page);
    const QSizeF size = m_doc ? m_doc->pageSize(page) : QSizeF();
    if (row.isEmpty() || !size.isValid() || size.width() <= 0) {
        return *m_cache.insert(page, QImage());
    }

    // Rendered at the screen's real density, like the page view — a soft
    // preview beside a sharp page would look like a mistake.
    const double ratio = renderRatio();
    const double scale = row.width() * ratio / size.width();
    QImage image = m_doc->renderPage(page, scale, Poppler::Page::Rotate0);
    if (m_night && !image.isNull()) {
        image = invertLightness(image);
    }
    if (!image.isNull()) {
        image.setDevicePixelRatio(ratio);
    }
    return *m_cache.insert(page, image);
}

void ThumbnailStrip::dropOutside(int first, int last) {
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        if (it.key() < first || it.key() > last) {
            it = m_cache.erase(it);
        } else {
            ++it;
        }
    }
}

void ThumbnailStrip::paintEvent(QPaintEvent *event) {
    QPainter painter(viewport());

    const QColor surround = palette().color(QPalette::Window);
    painter.fillRect(event->rect(), surround);

    if (!m_doc || !m_doc->isOpen() || m_rows.isEmpty()) {
        return;
    }

    if (!qFuzzyCompare(m_cacheRatio, renderRatio()) || m_cacheWidth != viewport()->width()) {
        m_cacheRatio = renderRatio();
        m_cacheWidth = viewport()->width();
        m_cache.clear();
    }

    const int offset = verticalScrollBar()->value();

    // Which rows are on screen. Everything else is neither drawn nor rendered,
    // which is the whole point of building this by hand.
    int first = m_rows.size();
    int last = -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        const QRect row = m_rows.at(i).adjusted(0, 0, 0, kLabelHeight);
        if (row.bottom() - offset >= 0 && row.top() - offset <= viewport()->height()) {
            first = qMin(first, i);
            last = qMax(last, i);
        }
    }
    if (last < 0) {
        return;
    }
    first = qMax(0, first - kRenderMargin);
    last = qMin(m_rows.size() - 1, last + kRenderMargin);
    dropOutside(first, last);

    const bool darkSurround = surround.lightness() < 128;
    // From a palette role, not named here — the same discipline the page view
    // is held to, so the strip follows the reader's theme rather than carrying
    // a second set of colour rules that could drift from it.
    QColor edge = palette().color(QPalette::Mid);
    if (darkSurround && edge.lightness() < surround.lightness()) {
        edge = palette().color(QPalette::Midlight);
    }
    const QColor label = palette().color(QPalette::WindowText);
    const QColor highlight = palette().color(QPalette::Highlight);

    painter.setRenderHint(QPainter::Antialiasing, true);

    for (int i = first; i <= last; ++i) {
        const QRect paper = m_rows.at(i).translated(0, -offset);

        // The page being read is named by a ring around its preview, drawn from
        // the palette's own highlight so it matches selection everywhere else.
        if (i == m_current) {
            QPen pen(highlight);
            pen.setWidth(2);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(paper.adjusted(-3, -3, 2, 2));
        }

        const QImage &image = thumbnailFor(i);
        if (image.isNull()) {
            // A page that will not render still shows as paper, so the strip
            // does not develop holes.
            painter.fillRect(paper, palette().color(QPalette::Base));
        } else {
            painter.drawImage(paper.topLeft(), image);
        }

        painter.setPen(edge);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(paper.adjusted(-1, -1, 0, 0));

        QColor number = label;
        number.setAlpha(i == m_current ? 255 : 150);
        painter.setPen(number);
        painter.drawText(QRect(paper.left(), paper.bottom() + 1, paper.width(), kLabelHeight),
                         Qt::AlignCenter, QString::number(i + 1));
    }
}

int ThumbnailStrip::pageAt(const QPoint &point) const {
    const int y = point.y() + verticalScrollBar()->value();
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).adjusted(0, 0, 0, kLabelHeight).contains(m_rows.at(i).left(), y)) {
            return i;
        }
    }
    return -1;
}

void ThumbnailStrip::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    const int page = pageAt(event->position().toPoint());
    if (page >= 0) {
        Q_EMIT pageChosen(page);
        event->accept();
        return;
    }
    QAbstractScrollArea::mousePressEvent(event);
}

} // namespace mergen
