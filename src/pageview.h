// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QAbstractScrollArea>
#include <QHash>
#include <QImage>
#include <QRect>
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

private:
    void relayout();
    void paintEmptyState(QPainter &painter);
    const QImage &cachedPage(int index);

    /// Gap between consecutive pages, and the margin around the column, in
    /// device pixels at any zoom.
    static constexpr int kPageGap = 12;

    Document *m_doc = nullptr;
    QString m_message;

    double m_zoom = 1.0;
    Poppler::Page::Rotation m_rotation = Poppler::Page::Rotate0;

    /// Page rectangles in content coordinates, one per laid-out page.
    QVector<QRect> m_layout;
    QSize m_content;
    QHash<int, QImage> m_cache;
};

} // namespace mergen
