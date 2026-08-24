// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

class QLabel;
class QScrollArea;
class QVBoxLayout;

namespace mergen {

/// The one transient surface MERGEN draws over the page.
///
/// MZ.md §5 bans persistent chrome, not chrome that is summoned and gone. This
/// is what that distinction is built on: a frameless panel over a dimmed page,
/// centred horizontally and offset from the top, sized to its content within a
/// fraction of the viewport, dismissed on Esc, on a click outside it, and on
/// choosing an entry. It is never docked, never resizable, and never survives
/// the document it was opened over.
///
/// It knows nothing about what it shows. Callers hand it a title and a widget;
/// the outline, the command overlay, properties and hold-to-peek are all the
/// same surface with different contents, which is what makes them behave
/// identically — a reader who learns Esc learns it once.
class Overlay : public QWidget {
    Q_OBJECT

public:
    explicit Overlay(QWidget *parent);

    /// Shows \a content under \a title, taking focus. Any previous content is
    /// destroyed. The overlay takes ownership of \a content.
    void present(const QString &title, QWidget *content);

    /// Hides the panel and returns focus where it was found.
    void dismiss();

    bool isPresented() const;

    /// Re-measures the panel against its content. Called when content changes
    /// size while the overlay is already up.
    void relayout();

Q_SIGNALS:
    void dismissed();

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *m_panel = nullptr;
    QLabel *m_title = nullptr;
    QScrollArea *m_scroll = nullptr;

    /// Where focus was before the overlay took it, so it can be handed back.
    QPointer<QWidget> m_focusBefore;
};

} // namespace mergen
