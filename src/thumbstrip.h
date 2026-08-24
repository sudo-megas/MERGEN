// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QAbstractScrollArea>
#include <QHash>
#include <QImage>
#include <QVector>

namespace mergen {

class Document;

/// A scrolling column of page previews down the left of the window.
///
/// Deliberately built the way PageView is built rather than as a QListWidget
/// full of icons: a thousand-page document must not render a thousand
/// thumbnails to show twelve. Only what is visible, plus a small margin, is
/// rasterised, and everything outside that window is dropped — the same
/// discipline, and for the same reason, as PageView's own cache.
class ThumbnailStrip : public QAbstractScrollArea {
    Q_OBJECT

public:
    explicit ThumbnailStrip(QWidget *parent = nullptr);

    /// Pass nullptr to empty the strip.
    void setDocument(Document *doc);

    /// Marks one page as the one being read, and scrolls it into view if it is
    /// not already there.
    void setCurrentPage(int page);
    int currentPage() const { return m_current; }

    /// Matches the page view, so the strip does not stay bright when the page
    /// beside it goes dark.
    void setNightMode(bool on);

Q_SIGNALS:
    /// The reader clicked a preview. MainWindow turns this into a jump.
    void pageChosen(int page);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    void relayout();
    /// The rasterised preview for one page, rendered on first use.
    const QImage &thumbnailFor(int page);
    void dropOutside(int first, int last);
    int pageAt(const QPoint &point) const;
    double renderRatio() const;
    /// The rectangle of one preview's paper, in content coordinates.
    QRect paperRect(int page) const;

    Document *m_doc = nullptr;
    int m_current = -1;
    bool m_night = false;

    /// One row per page: the paper, without the label beneath it.
    QVector<QRect> m_rows;
    int m_contentHeight = 0;
    QHash<int, QImage> m_cache;
    double m_cacheRatio = 1.0;
    /// The width the cache was rendered for. The strip is resizable, so this
    /// changes and the cache has to follow.
    int m_cacheWidth = 0;

    /// Breathing room around each preview, and between one and the next.
    static constexpr int kMargin = 10;
    /// Rows rendered beyond the visible ones, so scrolling does not reveal
    /// blanks before it reveals pages.
    static constexpr int kRenderMargin = 2;
    /// Height of the page number drawn under each preview.
    static constexpr int kLabelHeight = 16;
};

} // namespace mergen
