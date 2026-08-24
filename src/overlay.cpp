// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "overlay.h"

#include <QApplication>
#include <QEvent>
#include <QGraphicsDropShadowEffect>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QLayout>
#include <QScrollArea>
#include <QVBoxLayout>

namespace mergen {
namespace {

/// How much of the viewport the panel may take before its contents scroll.
constexpr double kMaxWidthFraction = 0.80;
constexpr double kMaxHeightFraction = 0.78;

/// Distance from the top of the page view to the top of the panel, as a
/// fraction of the viewport height. Offset rather than centred: a panel pinned
/// slightly high leaves the page readable behind it.
constexpr double kTopFraction = 0.10;

/// Alpha of the wash drawn over the page. Enough to push the page back without
/// hiding it — the reader is still reading.
constexpr int kDimAlpha = 96;

/// A floor on the panel width. Narrower than this and a properties table
/// wraps into a column of fragments, which reads as broken rather than tidy.
constexpr int kMinPanelWidth = 380;

constexpr int kPanelPadding = 18;
constexpr int kPanelRadius = 10;
constexpr int kShadowBlur = 40;

/// The floating panel. It paints its own rounded background rather than
/// letting the overlay paint one beneath it: the panel renders through a drop
/// shadow effect, which composites an offscreen pixmap of the panel and its
/// children, so anything the parent painted underneath does not come with it.
class Panel : public QWidget {
public:
    explicit Panel(QWidget *parent) : QWidget(parent) { setAutoFillBackground(false); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::Window));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), kPanelRadius,
                                kPanelRadius);
    }
};

} // namespace

Overlay::Overlay(QWidget *parent) : QWidget(parent) {
    hide();
    setFocusPolicy(Qt::StrongFocus);

    m_panel = new Panel(this);

    auto *shadow = new QGraphicsDropShadowEffect(m_panel);
    shadow->setBlurRadius(kShadowBlur);
    shadow->setOffset(0, 4);
    QColor shade = palette().color(QPalette::Shadow);
    shade.setAlpha(140);
    shadow->setColor(shade);
    m_panel->setGraphicsEffect(shadow);

    auto *column = new QVBoxLayout(m_panel);
    column->setContentsMargins(kPanelPadding, kPanelPadding, kPanelPadding, kPanelPadding);
    column->setSpacing(kPanelPadding / 2);

    m_title = new QLabel(m_panel);
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    column->addWidget(m_title);

    m_scroll = new QScrollArea(m_panel);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    // The panel paints its own background, so everything stacked on it must be
    // transparent: a viewport filled with Base draws a second, differently
    // toned rectangle inside the panel, which reads as a seam.
    m_scroll->setAutoFillBackground(false);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_scroll->setBackgroundRole(QPalette::NoRole);
    m_scroll->viewport()->setBackgroundRole(QPalette::NoRole);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    column->addWidget(m_scroll, 1);

    if (parent) {
        parent->installEventFilter(this);
    }

    // Focus moving outside the overlay closes it. A focus change to nothing is
    // the window being deactivated, which is a reader looking elsewhere for a
    // moment, not dismissing what they opened.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (!isVisible() || !now) {
            return;
        }
        if (now == this || isAncestorOf(now)) {
            return;
        }
        dismiss();
    });
}

bool Overlay::isPresented() const {
    return isVisible();
}

void Overlay::present(const QString &title, QWidget *content) {
    m_title->setText(title);
    m_title->setVisible(!title.isEmpty());

    // takeWidget hands the old content back unparented, so it must be destroyed
    // rather than leaked; setWidget alone would not free it.
    if (QWidget *previous = m_scroll->takeWidget()) {
        previous->deleteLater();
    }
    m_scroll->setWidget(content);
    // setWidget turns autoFillBackground on, which paints a second rectangle
    // over the panel's own background. Cleared here rather than in each caller,
    // so every surface that uses this overlay is seamless without knowing why.
    if (content) {
        content->setAutoFillBackground(false);
        content->setBackgroundRole(QPalette::NoRole);
    }

    if (!isVisible()) {
        m_focusBefore = QApplication::focusWidget();
    }

    if (parentWidget()) {
        setGeometry(parentWidget()->rect());
    }
    layoutPanel();
    show();
    raise();
    setFocus(Qt::OtherFocusReason);
}

void Overlay::dismiss() {
    if (!isVisible()) {
        return;
    }
    hide();
    if (QWidget *previous = m_focusBefore) {
        previous->setFocus(Qt::OtherFocusReason);
    } else if (parentWidget()) {
        parentWidget()->setFocus(Qt::OtherFocusReason);
    }
    m_focusBefore = nullptr;
    Q_EMIT dismissed();
}

void Overlay::layoutPanel() {
    if (!parentWidget()) {
        return;
    }
    const QSize viewport = parentWidget()->size();
    const int maxWidth = qRound(viewport.width() * kMaxWidthFraction);
    const int maxHeight = qRound(viewport.height() * kMaxHeightFraction);

    // Width is settled first and height derived from it. Word-wrapped labels
    // have no single sizeHint height — they have a height for a given width —
    // so asking for the hint before the width is known gives a panel that
    // scrolls content which would have fitted.
    const int chrome = 2 * kPanelPadding + m_title->sizeHint().height() + kPanelPadding / 2;

    int width = m_panel->sizeHint().width();
    int height = m_panel->sizeHint().height();

    if (QWidget *content = m_scroll->widget()) {
        // sizeHint through a scroll area reports the scroll area's own hint,
        // not the content's, so the content is measured directly.
        width = qBound(kMinPanelWidth, content->sizeHint().width() + 2 * kPanelPadding, maxWidth);

        const int forWidth = width - 2 * kPanelPadding;
        int contentHeight = -1;
        if (QLayout *layout = content->layout(); layout && layout->hasHeightForWidth()) {
            contentHeight = layout->heightForWidth(forWidth);
        }
        if (contentHeight <= 0) {
            contentHeight = content->sizeHint().height();
        }
        // A view's sizeHint is its own idea of a good size and ignores a height
        // fixed on it from outside — an item view asked to be exactly as tall as
        // its rows still reports the default. Honour the constraint.
        contentHeight = qBound(content->minimumHeight(), contentHeight, content->maximumHeight());
        height = contentHeight + chrome;
    }

    width = qBound(kMinPanelWidth, width, maxWidth);
    height = qBound(80, height, maxHeight);
    m_panel->setGeometry((viewport.width() - width) / 2, qRound(viewport.height() * kTopFraction),
                         width, height);
}

void Overlay::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Only the wash over the page. The panel paints itself — see Panel.
    QColor dim = palette().color(QPalette::Shadow);
    dim.setAlpha(kDimAlpha);
    painter.fillRect(rect(), dim);
}

void Overlay::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        dismiss();
        return;
    }
    QWidget::keyPressEvent(event);
}

void Overlay::mousePressEvent(QMouseEvent *event) {
    if (!m_panel->geometry().contains(event->pos())) {
        dismiss();
        return;
    }
    QWidget::mousePressEvent(event);
}

bool Overlay::eventFilter(QObject *watched, QEvent *event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible()) {
        setGeometry(parentWidget()->rect());
        layoutPanel();
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace mergen
