// MERGEN needs statx for a file's creation time; glibc gates it on _GNU_SOURCE.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "mainwindow.h"
#include "document.h"
#include "control.h"
#include "iconset.h"
#include "overlay.h"
#include "pageview.h"
#include "thumbstrip.h"
#include "redact.h"
#include "license.h"

#include <QAction>
#include <cstring>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontDatabase>
#include <QScopeGuard>
#include <QInputDialog>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QEvent>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QKeyEvent>
#include <QDialog>
#include <QFrame>
#include <QDialogButtonBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QSlider>
#include <cmath>
#include <QFontInfo>
#include <QDateTime>
#include <QStatusBar>
#include <QProgressDialog>
#include <QPrintDialog>
#include <QPrinter>
#include <QProgressBar>
#include <QPushButton>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QGridLayout>
#include <QMimeData>
#include <QListWidget>
#include <functional>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QThread>
#include <QVBoxLayout>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>

#include <fcntl.h>
#include <sys/stat.h>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>

#include <utility>

namespace mergen {
namespace {

constexpr int kRecentLimit = 10;

/// A ceiling on portals, for the same reason recent.toml has one: the whole
/// store is rewritten on every save, and an unbounded one is loaded in full at
/// every launch.
constexpr int kPortalLimit = 512;

/// Ceiling on print rendering. Above this the image cost climbs fast and the
/// paper does not improve.
constexpr int kPrintDpiCap = 600;

/// Alpha applied to the palette's highlight colour for the two interactive
/// states. Proportions, not colours: the hue is always the reader's own.
constexpr int kHoverAlpha = 46;
constexpr int kPressedAlpha = 82;
/// A checked toggle. Stronger than hover, because hover is a passing state and
/// this one has to hold the reader's attention while it stays on — it is the
/// only thing on the bar that says which mouse mode is active.
constexpr int kCheckedAlpha = 112;

/// A toolbar button that draws its own hover and pressed background.
///
/// v1.0 left this to whichever QStyle the reader happened to have, which meant
/// the surface they touch most looked different from desktop to desktop and,
/// under stock Fusion, looked like almost nothing at all. The wash here is
/// derived from QPalette::Highlight rather than named, so it follows the
/// reader's accent colour the way the selection and search colours already do
/// — MZ.md §6. Only the frame is ours; the contents stay the platform's.
class GlyphButton : public QToolButton {
public:
    explicit GlyphButton(QWidget *parent) : QToolButton(parent) {
        setAutoRaise(true);
        setFocusPolicy(Qt::NoFocus);
        // Without this Qt sends no paint event on mouse enter and leave, so the
        // hover state would never be drawn at all.
        setAttribute(Qt::WA_Hover, true);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QStylePainter painter(this);
        QStyleOptionToolButton option;
        initStyleOption(&option);

        const bool pressed = isEnabled() && (option.state & QStyle::State_Sunken);
        const bool hovered = isEnabled() && (option.state & QStyle::State_MouseOver);
        // State_On is the checked state of a checkable action. This painter used
        // to ignore it entirely, so a toggle that was on looked exactly like one
        // that was off — the button gave no sign which mouse mode was active.
        const bool checked = isEnabled() && (option.state & QStyle::State_On);

        if (pressed || hovered || checked) {
            QColor wash = palette().color(QPalette::Highlight);
            // A checked button being hovered or clicked should still respond, so
            // the states add rather than one replacing the other.
            int alpha = 0;
            if (checked) {
                alpha = kCheckedAlpha;
            }
            if (pressed) {
                alpha = qMax(alpha, kPressedAlpha) + (checked ? kHoverAlpha : 0);
            } else if (hovered) {
                alpha = qMax(alpha + (checked ? kHoverAlpha / 2 : 0), kHoverAlpha);
            }
            wash.setAlpha(qMin(alpha, 255));
            const qreal radius = height() * 0.18;
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(Qt::NoPen);
            painter.setBrush(wash);
            painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);

            // A checked toggle also carries an outline, so it survives a theme
            // whose highlight is close to the toolbar's own background — the
            // wash alone would be invisible there.
            if (checked) {
                QColor edge = palette().color(QPalette::Highlight);
                edge.setAlpha(200);
                painter.setPen(edge);
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius,
                                        radius);
            }
            painter.setRenderHint(QPainter::Antialiasing, false);
        }

        // With a dropdown the label owns only part of the button, and the arrow
        // is a separate primitive that drawing the label does not cover.
        const bool hasMenu = popupMode() == QToolButton::MenuButtonPopup;
        QStyleOptionToolButton label = option;
        if (hasMenu) {
            label.rect = style()->subControlRect(QStyle::CC_ToolButton, &option,
                                                 QStyle::SC_ToolButton, this);
        }
        painter.drawControl(QStyle::CE_ToolButtonLabel, label);

        if (hasMenu) {
            QStyleOption arrow = option;
            arrow.rect = style()->subControlRect(QStyle::CC_ToolButton, &option,
                                                 QStyle::SC_ToolButtonMenu, this);
            painter.drawPrimitive(QStyle::PE_IndicatorArrowDown, arrow);
        }
    }
};

// One string: compositor-drawn decorations have no separate zones to fill.
QString titleFor(const QString &fileName) {
    if (fileName.isEmpty()) {
        return QStringLiteral(R"(MERGEN ///— —\\\ MEGAS)");
    }
    return QStringLiteral(R"(MERGEN ///— %1 —\\\ MEGAS)").arg(fileName);
}

/// TOML basic-string escaping, enough for the one thing MERGEN writes.
QString tomlEscape(const QString &in) {
    QString out;
    out.reserve(in.size() + 8);
    for (const QChar c : in) {
        switch (c.unicode()) {
        case u'\\':
            out += QLatin1String("\\\\");
            break;
        case u'"':
            out += QLatin1String("\\\"");
            break;
        case u'\n':
            out += QLatin1String("\\n");
            break;
        case u'\r':
            out += QLatin1String("\\r");
            break;
        case u'\t':
            out += QLatin1String("\\t");
            break;
        default:
            out += c;
        }
    }
    return out;
}

QString tomlUnescape(const QString &in) {
    QString out;
    out.reserve(in.size());
    for (int i = 0; i < in.size(); ++i) {
        if (in.at(i) != QLatin1Char('\\') || i + 1 >= in.size()) {
            out += in.at(i);
            continue;
        }
        switch (in.at(++i).unicode()) {
        case u'n':
            out += QLatin1Char('\n');
            break;
        case u'r':
            out += QLatin1Char('\r');
            break;
        case u't':
            out += QLatin1Char('\t');
            break;
        case u'"':
            out += QLatin1Char('"');
            break;
        case u'\\':
            out += QLatin1Char('\\');
            break;
        default:
            // Unknown escape: a malformed file is not an error the user sees,
            // so keep the character and move on.
            out += in.at(i);
        }
    }
    return out;
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    m_doc = std::make_unique<Document>();

    auto *central = new QWidget(this);
    auto *column = new QVBoxLayout(central);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    m_viewRow = new QWidget(central);
    auto *row = new QHBoxLayout(m_viewRow);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(1);

    // The previews take about a fifth of the row, the page the rest. Stretch
    // rather than a fixed width, so the proportion survives any window size.
    m_thumbs = new ThumbnailStrip(m_viewRow);
    row->addWidget(m_thumbs, 1);

    m_view = new PageView(m_viewRow);
    m_overlay = new Overlay(m_view);
    row->addWidget(m_view, kViewStretch);

    connect(m_thumbs, &ThumbnailStrip::pageChosen, this,
            [this](int page) { m_view->scrollToPage(page); });

    buildSearchBar();
    column->addWidget(m_searchBar);
    column->addWidget(m_viewRow, 1);
    setCentralWidget(central);

    buildStatusBar();

    m_watcher = new QFileSystemWatcher(this);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &) {
        // Never reload behind the reader's back — offer, and wait to be asked.
        m_view->setNotice(tr("This file changed on disk. Click to reload."));
    });
    connect(m_view, &PageView::noticeClicked, this, &MainWindow::reloadDocument);
    connect(m_view, &PageView::pageChanged, this, &MainWindow::onPageChanged);
    connect(m_view, &PageView::pageChanged, this, [this](int page) {
        if (m_thumbs) {
            m_thumbs->setCurrentPage(page);
        }
    });
    connect(m_view, &PageView::searchHitsChanged, this, [this](int, int) { updateSearchStatus(); });
    connect(m_view, &PageView::linkPeekRequested, this, &MainWindow::peekPage);
    connect(m_view, &PageView::linkPeekEnded, this, [this] {
        if (m_overlay) {
            m_overlay->dismiss();
        }
    });

    setAcceptDrops(true);

    loadRecent();
    loadPortals();
    buildToolBar();
    updateTitle();
    onPageChanged(-1);
}

MainWindow::~MainWindow() {
    cancelSearch();
    if (m_searchThread) {
        // cancelSearch() above already set the worker's own flag; QThread's
        // interruption request would be read by nobody.
        m_searchThread->quit();
        // The bounded wait was the bug, not the safety net. SearchWorker checks
        // its cancel flag between pages, so one heavy page offers no
        // cancellation point and three seconds can expire with the thread still
        // running — after which ~QObject destroys a running QThread, which Qt
        // answers with qFatal. An abort on close, on a Release build, reported
        // by two parties.
        //
        // So the choice is not "wait three seconds or give up". It is wait, or
        // abort. Try the bounded wait first because it almost always suffices,
        // then wait properly: the remaining delay is one page's search, and a
        // window that takes a moment to close beats a window that crashes.
        if (!m_searchThread->wait(3000)) {
            m_searchThread->wait();
        }
    }
}

QToolButton *MainWindow::addGlyphAction(QAction *action, char16_t glyph, const QString &label,
                                        const QString &keyHint) {
    // The label is the action's text, and the glyph becomes its icon further
    // down in applyIcons(). In v1.0 the glyph was the text, which is what made
    // the button unreadable to assistive technology.
    action->setText(label);
    action->setIconText(label);

    // With no menu bar and a fixed toolbar, the tooltip is the only place a
    // shortcut is discoverable — MZ.md §6. keyHint carries the keys that are
    // handled in the page view rather than registered as shortcuts here.
    const QString keys =
        keyHint.isEmpty() ? action->shortcut().toString(QKeySequence::NativeText) : keyHint;
    action->setToolTip(keys.isEmpty() ? label : tr("%1  (%2)").arg(label, keys));

    m_glyphActions.append({action, glyph});

    auto *button = new GlyphButton(m_toolBar);
    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAccessibleName(label);
    m_toolBar->addWidget(button);
    return button;
}

void MainWindow::buildToolBar() {
    m_toolBar = addToolBar(QStringLiteral("MERGEN"));
    m_toolBar->setMovable(false);
    m_toolBar->setFloatable(false);
    m_toolBar->setContextMenuPolicy(Qt::PreventContextMenu);
    // The toolbar no longer carries the glyph font: glyphs are painted into
    // icons now, so the labels and the page counter use the reader's UI font.

    m_openAction = new QAction(this);
    m_openAction->setShortcut(QKeySequence::Open);
    connect(m_openAction, &QAction::triggered, this, &MainWindow::chooseFile);
    QToolButton *openButton = addGlyphAction(m_openAction, glyphs::kOpen, tr("Open"));
    m_recentMenu = new QMenu(openButton);
    openButton->setMenu(m_recentMenu);
    openButton->setPopupMode(QToolButton::MenuButtonPopup);
    rebuildRecentMenu();

    addToolBarGap();

    // F and Shift+F are handled in the page view's key handler, not as
    // window-wide shortcuts: a single-letter shortcut is dispatched before the
    // focused widget sees it, which would swallow every "f" typed into the
    // search field. The tooltip still names the key, since §6 documents it.
    m_fitWidthAction = new QAction(this);
    connect(m_fitWidthAction, &QAction::triggered, m_view, &PageView::setFitWidth);
    addGlyphAction(m_fitWidthAction, glyphs::kFitWidth, tr("Fit width"), QStringLiteral("F"));

    m_fitPageAction = new QAction(this);
    connect(m_fitPageAction, &QAction::triggered, m_view, &PageView::setFitPage);
    addGlyphAction(m_fitPageAction, glyphs::kFitPage, tr("Fit page"), QStringLiteral("Shift+F"));

    m_thumbsAction = new QAction(this);
    m_thumbsAction->setCheckable(true);
    m_thumbsAction->setChecked(true);
    m_thumbsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+B")));
    connect(m_thumbsAction, &QAction::triggered, this, [this](bool on) {
        if (m_thumbs) {
            m_thumbs->setVisible(on);
        }
    });
    // Icon only. The two application-level controls are secondary to the
    // document ones beside them, and the bar has to survive a narrow window
    // without folding the page counter into an overflow menu. The action still
    // carries its text for assistive technology — that is Z1's rule, and it is
    // about the action, not about what the button chooses to draw.
    m_panAction = new QAction(this);
    m_panAction->setCheckable(true);
    m_panAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+H")));
    connect(m_panAction, &QAction::triggered, this, [this](bool on) {
        m_view->setMouseMode(on ? MouseMode::Pan : MouseMode::Select);
        if (m_compareView) {
            m_compareView->setMouseMode(on ? MouseMode::Pan : MouseMode::Select);
        }
    });
    // The view can change the mode without the button — a middle-drag, or the
    // keybinding while the button is not focused — so the button follows it
    // rather than being the only thing that knows.
    connect(m_view, &PageView::mouseModeChanged, this,
            [this](MouseMode mode) { m_panAction->setChecked(mode == MouseMode::Pan); });
    // Icon only, like the other two toggles. A mode is confirmed by the pointer
    // changing shape, which is a better signal than a word on a button.
    QToolButton *panButton =
        addGlyphAction(m_panAction, glyphs::kPan, tr("Move page"), QStringLiteral("Ctrl+H"));
    panButton->setToolButtonStyle(Qt::ToolButtonIconOnly);

    QToolButton *thumbsButton = addGlyphAction(m_thumbsAction, glyphs::kThumbnails,
                                               tr("Page previews"), QStringLiteral("Ctrl+B"));
    thumbsButton->setToolButtonStyle(Qt::ToolButtonIconOnly);

    addToolBarGap();

    m_rotateAction = new QAction(this);
    m_rotateAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    connect(m_rotateAction, &QAction::triggered, m_view, &PageView::rotateClockwise);
    addGlyphAction(m_rotateAction, glyphs::kRotate, tr("Rotate"));

    m_searchAction = new QAction(this);
    m_searchAction->setShortcut(QKeySequence::Find);
    connect(m_searchAction, &QAction::triggered, this, &MainWindow::openSearch);
    addGlyphAction(m_searchAction, glyphs::kSearch, tr("Search"));

    m_printAction = new QAction(this);
    m_printAction->setShortcut(QKeySequence::Print);
    connect(m_printAction, &QAction::triggered, this, &MainWindow::printDocument);
    addGlyphAction(m_printAction, glyphs::kPrint, tr("Print"));

    addToolBarGap();

    // Zoom lives after the document controls: it is the one thing the reader
    // adjusts continuously, and it ends the bar so the slider can be long.
    m_zoomOutAction = new QAction(this);
    m_zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    connect(m_zoomOutAction, &QAction::triggered, m_view, &PageView::zoomOut);
    addGlyphAction(m_zoomOutAction, glyphs::kZoomOut, tr("Zoom out"));

    m_zoomInAction = new QAction(this);
    // QKeySequence::ZoomIn is Ctrl++, which most keyboards produce as Ctrl+=.
    m_zoomInAction->setShortcuts({QKeySequence::ZoomIn, QKeySequence(QStringLiteral("Ctrl+="))});
    connect(m_zoomInAction, &QAction::triggered, m_view, &PageView::zoomIn);
    addGlyphAction(m_zoomInAction, glyphs::kZoomIn, tr("Zoom in"));

    m_zoomSlider = new QSlider(Qt::Horizontal, m_toolBar);
    m_zoomSlider->setRange(0, kZoomTicks);
    // Expanding, and last in its group. It takes the bar's slack in place of the
    // blank spacer that used to sit here, so it is as long as the window allows
    // rather than as short as the bar could spare.
    m_zoomSlider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // A short minimum, because the bar must still fit a 1000px window with the
    // page counter visible. On any wider window the Expanding policy takes the
    // slack and the slider is as long as there is room for.
    m_zoomSlider->setMinimumWidth(48);
    // Capped, because "expanding" on a wide screen means 780px of zoom slider,
    // which is a control looking for something to do. Long enough to aim with.
    m_zoomSlider->setMaximumWidth(260);
    m_zoomSlider->setAccessibleName(tr("Zoom"));
    m_zoomSlider->setToolTip(tr("Zoom"));
    m_zoomSlider->setFocusPolicy(Qt::NoFocus); // Never steals the arrow keys.
    connect(m_zoomSlider, &QSlider::valueChanged, this, [this](int tick) {
        if (m_reflectingZoom) {
            return; // We put that value there ourselves.
        }
        m_view->setZoom(zoomForTick(tick));
    });
    m_zoomLabel = new QLabel(m_toolBar);
    m_zoomLabel->setAccessibleName(tr("Zoom level"));
    m_zoomLabel->setMinimumWidth(46);
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    m_toolBar->addWidget(m_zoomLabel);
    m_toolBar->addWidget(m_zoomSlider);

    // Whatever the slider does not take, so the page counter and About stay
    // pinned to the right edge.
    auto *slack = new QWidget(m_toolBar);
    slack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    slack->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_toolBar->addWidget(slack);

    connect(m_view, &PageView::zoomChanged, this, &MainWindow::updateZoomReadout);
    updateZoomReadout(m_view->zoom());

    // Counter-clockwise rotation is a keybinding only; §5 gives the toolbar one
    // rotate button.
    m_rotateBackAction = new QAction(this);
    m_rotateBackAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+R")));
    connect(m_rotateBackAction, &QAction::triggered, m_view, &PageView::rotateCounterClockwise);
    addAction(m_rotateBackAction);

    auto *resetZoomAction = new QAction(this);
    resetZoomAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    connect(resetZoomAction, &QAction::triggered, m_view, &PageView::resetZoom);
    addAction(resetZoomAction);

    auto *copyAction = new QAction(this);
    copyAction->setShortcut(QKeySequence::Copy);
    connect(copyAction, &QAction::triggered, m_view, &PageView::copySelection);
    addAction(copyAction);

    auto *closeSearchAction = new QAction(this);
    closeSearchAction->setShortcut(QKeySequence(QStringLiteral("Esc")));
    connect(closeSearchAction, &QAction::triggered, this, [this] {
        // Innermost first: a window-wide Esc is dispatched before the focused
        // widget sees it, so the overlay cannot close itself from its own key
        // handler while this exists.
        if (m_overlay && m_overlay->isPresented()) {
            m_overlay->dismiss();
            return;
        }
        if (m_searchBar && m_searchBar->isVisible()) {
            closeSearch();
            return;
        }
        if (m_view->isPresenting()) {
            setPresenting(false);
        }
    });
    addAction(closeSearchAction);

    // Enter and Shift+Enter are deliberately not window-wide shortcuts: Qt
    // dispatches shortcuts before the focused widget sees the key, so a global
    // Return would swallow Enter in the page counter and the search field.
    // They are handled where focus actually is — in the page view's key
    // handler and in the search field's event filter.

    auto *markAction = new QAction(this);
    markAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+M")));
    connect(markAction, &QAction::triggered, this, &MainWindow::markPortal);
    addAction(markAction);

    auto *followAction = new QAction(this);
    followAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+J")));
    connect(followAction, &QAction::triggered, this, &MainWindow::followPortal);
    addAction(followAction);

    auto *compareAction = new QAction(this);
    compareAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
    connect(compareAction, &QAction::triggered, this, [this] {
        if (isComparing()) {
            leaveCompare();
        } else {
            chooseComparison();
        }
    });
    addAction(compareAction);

    auto *presentAction = new QAction(this);
    presentAction->setShortcut(QKeySequence(QStringLiteral("F5")));
    connect(presentAction, &QAction::triggered, this,
            [this] { setPresenting(!m_view->isPresenting()); });
    addAction(presentAction);

    auto *nightAction = new QAction(this);
    nightAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+N")));
    connect(nightAction, &QAction::triggered, this, [this] {
        const bool on = !m_view->isNightMode();
        m_view->setNightMode(on);
        if (m_thumbs) {
            m_thumbs->setNightMode(on);
        }
    });
    addAction(nightAction);

    auto *commandAction = new QAction(this);
    commandAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
    connect(commandAction, &QAction::triggered, this, &MainWindow::showCommands);
    addAction(commandAction);

    auto *outlineAction = new QAction(this);
    outlineAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
    connect(outlineAction, &QAction::triggered, this, &MainWindow::showOutline);
    addAction(outlineAction);

    auto *propertiesAction = new QAction(this);
    propertiesAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+I")));
    connect(propertiesAction, &QAction::triggered, this, &MainWindow::showProperties);
    addAction(propertiesAction);

    // About has no toolbar button: §5 fixes the toolbar's contents, so it is
    // reached by the conventional help key instead.
    auto *aboutAction = new QAction(this);
    aboutAction->setShortcut(QKeySequence::HelpContents);
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
    addAction(aboutAction);

    m_quitAction = new QAction(this);
    m_quitAction->setShortcut(QKeySequence::Quit);
    connect(m_quitAction, &QAction::triggered, this, &MainWindow::close);
    addAction(m_quitAction);

    // Everything after this spacer is right-aligned.

    m_pageEdit = new QLineEdit(m_toolBar);
    m_pageEdit->setAlignment(Qt::AlignRight);
    m_pageEdit->setValidator(new QIntValidator(1, 1, m_pageEdit));
    m_pageEdit->setFixedWidth(m_pageEdit->fontMetrics().horizontalAdvance(QStringLiteral("00000")) +
                              16);
    m_pageEdit->setToolTip(tr("Page number  (type a page and press Enter)"));
    m_pageEdit->setAccessibleName(tr("Page number"));
    connect(m_pageEdit, &QLineEdit::returnPressed, this, &MainWindow::jumpToTypedPage);
    m_toolBar->addWidget(m_pageEdit);

    m_pageTotal = new QLabel(m_toolBar);
    m_pageTotal->setAccessibleName(tr("Page count"));
    m_toolBar->addWidget(m_pageTotal);

    addToolBarGap();

    // Last on the bar, so it sits at the top right under the window's own close
    // button. It is the only control here that is about the application rather
    // than about the document, and it belongs at the end for that reason.
    m_aboutAction = new QAction(this);
    connect(m_aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
    QToolButton *aboutButton = addGlyphAction(m_aboutAction, glyphs::kAbout, tr("About"));
    aboutButton->setToolButtonStyle(Qt::ToolButtonIconOnly);

    // Zoom bounds decide whether the two zoom buttons are still live.
    connect(m_view, &PageView::zoomChanged, this, [this](double) { updateActionStates(); });

    applyIcons();
    updateActionStates();
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    // The path is elided to the label's width, so a wider window should show
    // more of it rather than keep the cut made for the old one.
    updateStatusBar();
}

void MainWindow::buildStatusBar() {
    // What the reader has open, and what they are allowed to do with it. The
    // path because a file name alone is ambiguous once two directories hold a
    // report.pdf; the mode because this application can open files the account
    // cannot write, and knowing that before trying to save is worth a line.
    auto *bar = statusBar();
    bar->setSizeGripEnabled(false);
    // QStatusBar draws a frame around every item it holds, which at this size
    // is a box round each word. The bar is one line of text, not a row of
    // panels.
    bar->setStyleSheet(QStringLiteral("QStatusBar::item { border: none; }"));
    bar->setContentsMargins(kStatusPad, 0, kStatusPad, 0);

    // Qt gives the status bar a font a size or two below the interface's, which
    // is how a path and a permission string end up unreadable. Both are worth
    // reading, so both are set to the interface font outright.
    QFont uiFont = QApplication::font();
    uiFont.setPointSizeF(uiFont.pointSizeF() * kStatusScale);
    uiFont.setBold(true);

    // The system's monospace font is not necessarily one that can be bold. Here
    // it resolves to Andale Mono, which ships no bold face, and Qt does not
    // synthesise one — asking for weight 700 gave back glyphs identical to
    // weight 400, pixel for pixel and to the same advance width. So the family
    // is chosen by whether it can actually do what is being asked of it.
    QFont fixedFont = boldMonospaceFont();
    fixedFont.setPointSizeF(uiFont.pointSizeF());
    fixedFont.setBold(true);

    // The dates carry two lines, so they run a little smaller than the single
    // line either side of them and the three sections come out the same height.
    QFont dateFont = QApplication::font();
    dateFont.setPointSizeF(dateFont.pointSizeF() * kStatusScale * 0.86);
    dateFont.setBold(true);

    m_statusPath = new QLabel(bar);
    m_statusPath->setTextFormat(Qt::PlainText);
    m_statusPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusPath->setAccessibleName(tr("File path"));
    m_statusPath->setFont(uiFont);
    bar->addWidget(m_statusPath, 1);

    // Centred between the path and the permissions, with equal stretch on each
    // side so it stays in the middle rather than drifting with the path's
    // length. Two lines, because the bar has the room for them.
    m_statusDates = new QLabel(bar);
    m_statusDates->setTextFormat(Qt::PlainText);
    m_statusDates->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusDates->setAccessibleName(tr("File dates"));
    m_statusDates->setFont(dateFont);
    m_statusDates->setAlignment(Qt::AlignCenter);
    bar->addWidget(m_statusDates, 0);

    // Octal and symbolic sit together at the right, in one monospaced run so
    // the columns line up from one document to the next.
    m_statusMode = new QLabel(bar);
    m_statusMode->setTextFormat(Qt::PlainText);
    m_statusMode->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusMode->setAccessibleName(tr("File permissions"));
    m_statusMode->setFont(fixedFont);
    m_statusMode->setToolTip(tr("The file's permissions, as ls reports them"));
    m_statusMode->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // In the same flow as the path, with the same stretch, rather than as a
    // permanent widget. A permanent widget is laid out after the stretch is
    // shared, so the space either side of the dates was unequal and they sat
    // 74px left of centre on a 1400px window.
    bar->addWidget(m_statusMode, 1);

    // Twice the single-line height it was, which is what the two date lines
    // need and what makes the bar read as part of the window rather than as a
    // strip left over at the bottom.
    bar->setFixedHeight(2 * (QFontMetrics(uiFont).height() + kStatusPad));

    updateStatusBar();
}

QFont MainWindow::boldMonospaceFont() {
    // Cascadia first: it is already a hard dependency of this package, because
    // the toolbar's icons are glyphs from it, so preferring it adds nothing to
    // install. Then the system's own choice, then the usual Linux monospaces.
    const QStringList candidates = {
        QStringLiteral("CaskaydiaCove Nerd Font"),
        QStringLiteral("Cascadia Code NF"),
        QStringLiteral("Cascadia Code"),
        QFontInfo(QFontDatabase::systemFont(QFontDatabase::FixedFont)).family(),
        QStringLiteral("DejaVu Sans Mono"),
        QStringLiteral("Liberation Mono"),
        QStringLiteral("Noto Sans Mono"),
    };

    const QStringList installed = QFontDatabase::families();
    for (const QString &family : candidates) {
        if (family.isEmpty() || !installed.contains(family)) {
            continue;
        }
        // Both conditions matter: a proportional font would lose the column
        // alignment that makes two permission strings comparable at a glance,
        // and a family with no bold face is the problem being solved.
        const QFont probe(family);
        if (!QFontInfo(probe).fixedPitch()) {
            continue;
        }
        if (!QFontDatabase::styles(family).contains(QStringLiteral("Bold"), Qt::CaseInsensitive)) {
            continue;
        }
        return QFont(family);
    }

    // Nothing monospaced can be bold on this system. Bold was asked for
    // explicitly and column alignment was not, so the interface font wins.
    return QApplication::font();
}

QString MainWindow::dateText(const QString &path) {
    const auto when = [](qint64 seconds) {
        return QDateTime::fromSecsSinceEpoch(seconds).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    };

    // statx rather than stat, because stat has no creation time to give: st_ctime
    // is the inode's *change* time, which moves when permissions change and is
    // routinely mistaken for this. Not every filesystem records a birth time, so
    // the mask has to be checked rather than assumed — the field is present in
    // the struct either way and holds nothing meaningful when unset.
    struct statx sx{};
    if (::statx(AT_FDCWD, path.toLocal8Bit().constData(), 0, STATX_BTIME | STATX_MTIME, &sx) == 0) {
        const QString modified =
            (sx.stx_mask & STATX_MTIME) ? when(qint64(sx.stx_mtime.tv_sec)) : QString();
        if (sx.stx_mask & STATX_BTIME) {
            return tr("Created   %1\nModified  %2")
                .arg(when(qint64(sx.stx_btime.tv_sec)), modified);
        }
        if (!modified.isEmpty()) {
            // Said plainly rather than filled in with the change time, which
            // would be a different fact wearing this one's label.
            return tr("Created   not recorded\nModified  %1").arg(modified);
        }
    }

    struct stat info{};
    if (::stat(path.toLocal8Bit().constData(), &info) == 0) {
        return tr("Created   not recorded\nModified  %1").arg(when(qint64(info.st_mtime)));
    }
    return QString();
}

QString MainWindow::permissionText(const QString &path) {
    struct stat info{};
    if (::stat(path.toLocal8Bit().constData(), &info) != 0) {
        return QString();
    }

    // The ls form, built from the mode bits rather than from Qt's permission
    // flags — Qt reports what *this process* may do, which is not the same
    // question and answers it wrongly for a file opened through pkexec.
    const mode_t mode = info.st_mode;
    QString rwx;
    rwx.reserve(10);
    rwx += S_ISDIR(mode) ? QLatin1Char('d') : S_ISLNK(mode) ? QLatin1Char('l') : QLatin1Char('-');
    const mode_t bits[9] = {S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP,
                            S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH};
    const char letters[9] = {'r', 'w', 'x', 'r', 'w', 'x', 'r', 'w', 'x'};
    for (int i = 0; i < 9; ++i) {
        rwx += (mode & bits[i]) ? QLatin1Char(letters[i]) : QLatin1Char('-');
    }
    // setuid, setgid and sticky replace the execute letter, as ls shows them.
    const auto special = [&rwx, mode](int index, mode_t bit, char set, char unset) {
        if (mode & bit) {
            rwx[index] = (mode & (index == 3   ? S_IXUSR
                                  : index == 6 ? S_IXGRP
                                               : S_IXOTH))
                             ? QLatin1Char(set)
                             : QLatin1Char(unset);
        }
    };
    special(3, S_ISUID, 's', 'S');
    special(6, S_ISGID, 's', 'S');
    special(9, S_ISVTX, 't', 'T');

    const uint octal = mode & (S_ISUID | S_ISGID | S_ISVTX | 0777);
    return QStringLiteral("%1  %2")
        .arg(octal, (mode & (S_ISUID | S_ISGID | S_ISVTX)) ? 4 : 4, 8, QLatin1Char('0'))
        .arg(rwx);
}

void MainWindow::updateStatusBar() {
    if (!m_statusPath || !m_statusMode || !m_statusDates) {
        return;
    }
    if (!m_doc->isOpen() && !m_doc->isLocked()) {
        m_statusPath->setText(QString());
        m_statusMode->setText(QString());
        m_statusDates->setText(QString());
        return;
    }

    const QString path = m_doc->path();
    // Elided in the middle: the beginning says where in the tree it is and the
    // end says which file it is, and a deep path would otherwise push the
    // permissions off the bar entirely.
    const QFontMetrics metrics(m_statusPath->font());
    m_statusPath->setText(
        metrics.elidedText(path, Qt::ElideMiddle, qMax(80, m_statusPath->width())));
    m_statusPath->setToolTip(path);

    // Both of these stat() the file as *this* account, which fails for exactly
    // the documents the elevation helper exists to open: the reader
    // authenticated to read the bytes, they did not gain the right to inspect
    // the file afterwards. Two empty labels look like a status bar that is
    // broken rather than one that has answered honestly, so the reason is
    // written where the value would be.
    const QString mode = permissionText(path);
    const QString dates = dateText(path);
    const bool elevated = !m_doc->data().isEmpty();
    const QString unavailable = elevated ? tr("needs privilege") : tr("not readable");

    m_statusMode->setText(mode.isEmpty() ? unavailable : mode);
    m_statusMode->setToolTip(mode.isEmpty() ? tr("This account cannot read the file's permissions. "
                                                 "It was opened with elevated permission.")
                                            : tr("The file's permissions, as ls reports them"));

    m_statusDates->setText(dates.isEmpty() ? tr("Created   %1\nModified  %1").arg(unavailable)
                                           : dates);
}

void MainWindow::addToolBarGap() {
    // A gap rather than QToolBar::addSeparator. The separator draws a hairline
    // rule, and at this icon size a stack of them reads as damage rather than as
    // grouping — whitespace groups perfectly well on its own.
    auto *gap = new QWidget(m_toolBar);
    gap->setFixedWidth(kToolBarGap);
    gap->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_toolBar->addWidget(gap);
}

void MainWindow::applyIcons() {
    if (!m_toolBar) {
        return;
    }
    const int extent = m_toolBar->iconSize().width();
    for (const auto &entry : std::as_const(m_glyphActions)) {
        // A null icon when the font is missing leaves the label standing alone,
        // which is the v1.0 behaviour and better than a tofu box.
        entry.first->setIcon(IconSet::icon(entry.second, extent, palette()));
    }
    if (m_searchCancel) {
        m_searchCancel->setIcon(IconSet::icon(glyphs::kClose, extent, palette()));
    }
}

void MainWindow::updateActionStates() {
    const bool open = m_doc && m_doc->isOpen();

    for (QAction *action : {m_fitWidthAction, m_fitPageAction, m_rotateAction, m_rotateBackAction,
                            m_searchAction, m_printAction}) {
        if (action) {
            action->setEnabled(open);
        }
    }

    // Comparing doubles that were reached by repeated stepping needs slack, and
    // half a step is far tighter than any step yet still never misses a bound.
    constexpr double kSlack = PageView::kZoomStep / 2.0;
    const double zoom = m_view ? m_view->zoom() : 1.0;
    if (m_zoomInAction) {
        m_zoomInAction->setEnabled(open && zoom < PageView::kMaxZoom - kSlack);
    }
    if (m_zoomOutAction) {
        m_zoomOutAction->setEnabled(open && zoom > PageView::kMinZoom + kSlack);
    }
}

void MainWindow::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::ThemeChange) {
        // Every cached pixmap was tinted for the palette that just went away.
        IconSet::clearCache();
        applyIcons();
    }
}

void MainWindow::openPath(const QString &path) {
    // Authentication and password prompts both run nested event loops, which
    // leaves the toolbar live; a second open must not re-enter this.
    if (m_opening) {
        return;
    }
    m_opening = true;
    const QScopeGuard done([this] { m_opening = false; });

    // A comparison belongs to the pair that started it. Opening anything else
    // — by dialog, drop, socket, recent list or portal — ends it, rather than
    // leaving two unrelated documents side by side with the old marks on them.
    leaveCompare();

    const QFileInfo info(path);
    const QString name = info.fileName();

    LoadStatus status = m_doc->openPath(path);

    if (status == LoadStatus::NoPermission) {
        // The file exists but is closed to this user. Offer to read it with
        // privilege rather than reporting a dead end.
        QString error;
        const QByteArray bytes = readElevated(info.absoluteFilePath(), &error);
        if (bytes.isEmpty()) {
            showError(error.isEmpty() ? tr("%1 cannot be read.").arg(name) : error);
            return;
        }
        status = m_doc->openData(bytes, info.absoluteFilePath());
    }

    if (status == LoadStatus::NeedsPassword && !promptForPassword(name)) {
        showError(QString());
        return;
    }
    if (status == LoadStatus::NeedsPassword) {
        status = LoadStatus::Ok;
    }

    switch (status) {
    case LoadStatus::Ok:
        break;
    case LoadStatus::NotFound:
        showError(tr("%1 does not exist.").arg(name));
        return;
    case LoadStatus::NoPermission:
        showError(tr("%1 cannot be read.").arg(name));
        return;
    case LoadStatus::Invalid:
        showError(tr("%1 is not a PDF, or it is damaged.").arg(name));
        return;
    case LoadStatus::NeedsPassword:
        showError(QString());
        return;
    }

    closeSearch();
    m_view->setDocument(m_doc.get());
    if (m_thumbs) {
        m_thumbs->setDocument(m_doc.get());
    }
    updateStatusBar();
    m_view->setNotice(QString());
    updateTitle();
    pushRecent(m_doc->path());
    watchDocument(m_doc->path());
}

void MainWindow::showError(const QString &message) {
    closeDocument(message);
}

void MainWindow::closeDocument(const QString &message) {
    if (m_overlay) {
        m_overlay->dismiss();
    }
    leaveCompare();
    closeSearch();
    m_doc->close();
    m_view->setDocument(nullptr);
    if (m_thumbs) {
        m_thumbs->setDocument(nullptr);
    }
    updateStatusBar();
    m_view->setNotice(QString());
    m_view->setMessage(message);
    if (!m_watcher->files().isEmpty()) {
        m_watcher->removePaths(m_watcher->files());
    }
    updateTitle();
    onPageChanged(-1);
}

double MainWindow::zoomForTick(int tick) const {
    // Logarithmic, so the slider's middle is 100% rather than 500%. A linear
    // map over 10%..1000% would spend nine tenths of its travel above life size.
    const double lo = std::log(PageView::kMinZoom);
    const double hi = std::log(PageView::kMaxZoom);
    const double t = double(qBound(0, tick, kZoomTicks)) / kZoomTicks;
    return std::exp(lo + t * (hi - lo));
}

int MainWindow::tickForZoom(double zoom) const {
    const double lo = std::log(PageView::kMinZoom);
    const double hi = std::log(PageView::kMaxZoom);
    const double clamped = qBound(PageView::kMinZoom, zoom, PageView::kMaxZoom);
    return qRound(kZoomTicks * (std::log(clamped) - lo) / (hi - lo));
}

void MainWindow::updateZoomReadout(double zoom) {
    if (m_zoomLabel) {
        // The true value, not the slider's clamped one: a fit mode on a very
        // large page legitimately lands below kMinZoom, and the reader should
        // be told what they are actually looking at.
        m_zoomLabel->setText(tr("%1%").arg(qRound(zoom * 100.0)));
    }
    if (m_zoomSlider) {
        // The slider has no position for a zoom below its range, so it rests at
        // the bottom. Guarded, because moving the handle emits valueChanged and
        // that would write the clamp back into the view.
        m_reflectingZoom = true;
        m_zoomSlider->setValue(tickForZoom(zoom));
        m_reflectingZoom = false;
    }
}

void MainWindow::updateTitle() {
    const QString name = m_doc->isOpen() ? QFileInfo(m_doc->path()).fileName() : QString();
    setWindowTitle(titleFor(name));
}

void MainWindow::chooseFile() {
    const QString start =
        m_doc->isOpen() ? QFileInfo(m_doc->path()).absolutePath() : QDir::homePath();
    const QString chosen =
        QFileDialog::getOpenFileName(this, tr("Open PDF"), start, tr("PDF documents (*.pdf)"));
    if (!chosen.isEmpty()) {
        openPath(chosen);
    }
}

void MainWindow::reloadDocument() {
    if (!m_doc->isOpen()) {
        return;
    }
    const QString path = m_doc->path();
    m_view->setNotice(QString());
    openPath(path);
}

void MainWindow::onPageChanged(int index) {
    updateActionStates();
    const int count = m_doc->pageCount();
    if (!m_doc->isOpen() || count <= 0) {
        m_pageEdit->clear();
        m_pageEdit->setEnabled(false);
        m_pageTotal->setText(QStringLiteral(" / 0"));
        return;
    }
    m_pageEdit->setEnabled(true);
    static_cast<QIntValidator *>(const_cast<QValidator *>(m_pageEdit->validator()))->setTop(count);
    m_pageTotal->setText(QStringLiteral(" / %1").arg(count));
    if (!m_pageEdit->hasFocus()) {
        m_pageEdit->setText(QString::number(qMax(0, index) + 1));
    }
}

void MainWindow::jumpToTypedPage() {
    if (!m_doc->isOpen()) {
        return;
    }
    bool ok = false;
    const int page = m_pageEdit->text().toInt(&ok);
    if (!ok || page < 1 || page > m_doc->pageCount()) {
        // Put the field back to where the reader actually is. Set it directly
        // rather than through onPageChanged, which leaves a focused field
        // alone so that scrolling cannot overwrite what is being typed.
        m_pageEdit->setText(QString::number(qMax(0, m_view->currentPage()) + 1));
        m_pageEdit->selectAll();
        return;
    }
    m_view->scrollToPage(page - 1);
    m_view->setFocus();
}

bool MainWindow::promptForPassword(const QString &fileName) {
    forever {
        bool accepted = false;
        QString typed =
            QInputDialog::getText(this, tr("MERGEN"), tr("Password for %1:").arg(fileName),
                                  QLineEdit::Password, QString(), &accepted);
        if (!accepted) {
            typed.fill(QLatin1Char('\0'));
            return false;
        }
        QByteArray password = typed.toUtf8();
        typed.fill(QLatin1Char('\0'));
        const bool opened = m_doc->unlock(password);
        // The password lives no longer than the attempt: MX.md §5.
        password.fill('\0');
        if (opened) {
            return true;
        }
    }
}

QByteArray MainWindow::readElevated(const QString &path, QString *error) const {
    const QString name = QFileInfo(path).fileName();

    QProcess pkexec;
    // Absolute: a bare name is resolved through $PATH, and a shim there
    // replaces the whole privileged read with no prompt at all.
    pkexec.setProgram(QStringLiteral("/usr/bin/pkexec"));
    pkexec.setArguments({QStringLiteral(MERGEN_HELPER_PATH), path});
    pkexec.setProcessChannelMode(QProcess::SeparateChannels);
    pkexec.start();

    if (!pkexec.waitForStarted(5000)) {
        *error = tr("%1 cannot be read, and pkexec is not available.").arg(name);
        return QByteArray();
    }
    // The authentication prompt belongs to the agent, not to MERGEN, and the
    // reader may take a while over it. Spin a local event loop rather than
    // blocking, so the window carries on painting instead of going grey.
    if (pkexec.state() != QProcess::NotRunning) {
        QEventLoop loop;
        connect(&pkexec, &QProcess::finished, &loop, &QEventLoop::quit);
        loop.exec();
    }

    const int code = pkexec.exitCode();
    if (pkexec.exitStatus() != QProcess::NormalExit || code != 0) {
        // pkexec's own failures: 126 is dismissed or refused, 127 is no agent.
        if (code == 126) {
            *error = tr("%1 was not opened: authorisation was declined.").arg(name);
        } else if (code == 127) {
            *error = tr("%1 cannot be read, and no authentication agent is running.").arg(name);
        } else {
            *error = tr("%1 cannot be read.").arg(name);
        }
        return QByteArray();
    }

    QByteArray bytes = pkexec.readAllStandardOutput();
    if (bytes.isEmpty()) {
        *error = tr("%1 cannot be read.").arg(name);
        return QByteArray();
    }

    // The helper checks the magic before it sends anything, so its absence here
    // means the stream was not the helper's — a shim on the path, a truncated
    // pipe, a wrapper that printed something of its own. Refusing costs an
    // authorisation the reader already gave; accepting hands poppler bytes of
    // unknown origin obtained with root.
    if (!bytes.startsWith("%PDF-")) {
        std::memset(bytes.data(), 0, static_cast<size_t>(bytes.size()));
        bytes.clear();
        *error = tr("%1 cannot be read.").arg(name);
        return QByteArray();
    }
    return bytes;
}

void MainWindow::watchDocument(const QString &path) {
    if (!m_watcher->files().isEmpty()) {
        m_watcher->removePaths(m_watcher->files());
    }
    if (!path.isEmpty()) {
        // Watching needs read permission, so a file opened through the helper
        // is not watched. The reload notice simply never appears — see §5.
        m_watcher->addPath(path);
    }
}

// --- Printing --------------------------------------------------------------

bool MainWindow::listenForCommands() {
    m_control = new Control(
        [this](const QString &verb, const QString &argument) { return runCommand(verb, argument); },
        this);
    return m_control->listen();
}

QString MainWindow::runCommand(const QString &verb, const QString &argument) {
    const QString ok = QStringLiteral("ok");
    const auto err = [](const QString &why) { return QStringLiteral("err: ") + why; };

    // Belt as well as braces. The print loop already excludes socket notifiers
    // so nothing should arrive here mid-print, but a command that changed the
    // document under a running print would corrupt the output silently, and
    // this costs one comparison.
    if (m_printing) {
        return err(QStringLiteral("busy printing"));
    }

    if (verb == QLatin1String("open")) {
        if (argument.isEmpty()) {
            return err(QStringLiteral("open needs a path"));
        }
        // openPath refuses to re-enter itself while a prompt is up, and used to
        // return quietly — so the socket answered "ok" to an open that never
        // happened, for any path at all.
        if (m_opening) {
            return err(QStringLiteral("busy opening another document"));
        }
        openPath(argument);
        // Raising is the point of handing a file to a running instance: the
        // reader asked for this document to be in front of them.
        raise();
        activateWindow();
        return m_doc->isOpen() ? ok : err(QStringLiteral("could not open ") + argument);
    }
    if (verb == QLatin1String("goto")) {
        bool number = false;
        const int page = argument.toInt(&number);
        if (!number) {
            return err(QStringLiteral("goto needs a page number"));
        }
        if (!m_doc->isOpen()) {
            return err(QStringLiteral("no document"));
        }
        if (page < 1 || page > m_doc->pageCount()) {
            return err(QStringLiteral("no page %1").arg(page));
        }
        m_view->scrollToPage(page - 1);
        return ok;
    }
    if (verb == QLatin1String("search")) {
        if (!m_doc->isOpen()) {
            return err(QStringLiteral("no document"));
        }
        if (argument.isEmpty()) {
            return err(QStringLiteral("search needs a term"));
        }
        openSearch();
        m_searchEdit->setText(argument);
        startSearch();
        return ok;
    }
    if (verb == QLatin1String("next") || verb == QLatin1String("prev")) {
        if (m_view->searchHitCount() == 0) {
            return err(QStringLiteral("no search hits"));
        }
        if (verb == QLatin1String("next")) {
            m_view->nextSearchHit();
        } else {
            m_view->previousSearchHit();
        }
        updateSearchStatus();
        return ok;
    }
    if (verb == QLatin1String("quit")) {
        // Answer before leaving, so the caller is not left waiting on a socket
        // that is about to close.
        QMetaObject::invokeMethod(this, &MainWindow::close, Qt::QueuedConnection);
        return ok;
    }
    return err(QStringLiteral("unknown command ") + verb);
}

QString MainWindow::portalFilePath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) +
           QStringLiteral("/mergen/portals.toml");
}

void MainWindow::loadPortals() {
    m_portals.clear();
    QFile file(portalFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    // A portal is read as a record rather than by counting keys. The previous
    // loader advanced from one end to the other only when it saw "page", so a
    // reordered, missing or duplicated line silently destroyed both ends —
    // permanently, on the next save.
    QVector<PortalEnd> ends;
    PortalEnd building;
    bool started = false;
    int seen = 0;

    const auto finishEnd = [&] {
        if (started && seen > 0 && ends.size() < 2) {
            ends.append(building);
        }
        building = PortalEnd();
        started = false;
        seen = 0;
    };
    const auto finishPortal = [&] {
        finishEnd();
        // Both ends or neither: a half-written portal goes nowhere, and
        // keeping it would be keeping a link with one side missing.
        if (ends.size() == 2) {
            m_portals.append({ends.at(0), ends.at(1)});
        }
        ends.clear();
    };

    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line == QLatin1String("[[portal]]")) {
            finishPortal();
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq < 0) {
            continue;
        }
        const QString key = line.left(eq).trimmed();
        QString value = line.mid(eq + 1).trimmed();
        if (value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')) &&
            value.size() >= 2) {
            value = tomlUnescape(value.mid(1, value.size() - 2));
        }

        // A key repeating means the previous end is complete, whatever order
        // its lines arrived in.
        const bool repeat = (key == QLatin1String("hash") && !building.hash.isEmpty()) ||
                            (key == QLatin1String("path") && !building.path.isEmpty()) ||
                            (key == QLatin1String("page") && seen > 0 &&
                             building.hash.isEmpty() == building.path.isEmpty() && seen >= 3);
        if (repeat) {
            finishEnd();
        }

        if (key == QLatin1String("hash")) {
            building.hash = value;
        } else if (key == QLatin1String("path")) {
            building.path = value;
        } else if (key == QLatin1String("page")) {
            building.page = value.toInt();
        } else {
            continue;
        }
        started = true;
        ++seen;
        if (seen >= 3) {
            finishEnd();
        }
    }
    finishPortal();

    // Bounded, the way recent.toml is. A store that only grows is a store that
    // is eventually rewritten in full on every save.
    if (m_portals.size() > kPortalLimit) {
        m_portals.remove(0, m_portals.size() - kPortalLimit);
    }
}

bool MainWindow::savePortals() const {
    const QString path = portalFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    QByteArray out;
    for (const Portal &portal : m_portals) {
        out += "[[portal]]\n";
        for (const PortalEnd *end : {&portal.a, &portal.b}) {
            out += QStringLiteral("hash = \"%1\"\n").arg(tomlEscape(end->hash)).toUtf8();
            out += QStringLiteral("path = \"%1\"\n").arg(tomlEscape(end->path)).toUtf8();
            out += QStringLiteral("page = %1\n").arg(end->page).toUtf8();
        }
    }
    file.write(out);
    // The reader is told what actually happened, not what was attempted.
    return file.commit();
}

void MainWindow::markPortal() {
    if (!m_doc->isOpen()) {
        return;
    }
    const PortalEnd here{m_doc->contentHash(), m_doc->path(), m_view->currentPage()};
    if (here.hash.isEmpty()) {
        return;
    }

    if (!m_pendingEnd) {
        m_pendingEnd = here;
        m_view->setNotice(tr("Portal started here. Go to the other end and press Ctrl+M again."));
        return;
    }
    if (m_pendingEnd->hash == here.hash && m_pendingEnd->page == here.page) {
        // Both ends in the same place is not a link, it is a cancel.
        m_pendingEnd.reset();
        m_view->setNotice(QString());
        return;
    }

    m_portals.append({*m_pendingEnd, here});
    m_pendingEnd.reset();
    if (savePortals()) {
        m_view->setNotice(tr("Portal made. Ctrl+J follows it."));
    } else {
        // It exists for this session, but it will not be there tomorrow.
        m_view->setNotice(tr("Portal made, but it could not be saved to disk."));
    }
}

void MainWindow::followPortal() {
    if (!m_doc->isOpen()) {
        return;
    }
    const QString hash = m_doc->contentHash();
    const int page = m_view->currentPage();

    for (const Portal &portal : m_portals) {
        const PortalEnd *from = nullptr;
        const PortalEnd *to = nullptr;
        if (portal.a.hash == hash && portal.a.page == page) {
            from = &portal.a;
            to = &portal.b;
        } else if (portal.b.hash == hash && portal.b.page == page) {
            from = &portal.b;
            to = &portal.a;
        }
        if (!from) {
            continue;
        }

        if (to->hash == hash) {
            m_view->scrollToPage(to->page);
            return;
        }
        if (!QFileInfo::exists(to->path)) {
            // Kept, not deleted: the file may simply not be mounted today.
            m_view->setNotice(tr("The other end of this portal is in %1, which is not there.")
                                  .arg(QFileInfo(to->path).fileName()));
            return;
        }
        // Copied out before openPath, not read through `to` after it: `to`
        // points into m_portals, and openPath's call graph is long enough that
        // "nothing in it reloads the portal list" is a promise better kept by
        // not needing it.
        const int target = to->page;
        const QString expected = to->hash;
        const QString farPath = to->path;
        openPath(farPath);
        if (!m_doc->isOpen()) {
            return;
        }
        // A path is not an identity. The file at the far end may have been
        // replaced since the portal was made — same name, different document —
        // and jumping to page 40 of a stranger looks exactly like working.
        // The hash was recorded for this; use it rather than trusting the path.
        if (!expected.isEmpty() && m_doc->contentHash() != expected) {
            m_view->setNotice(tr("%1 is not the document this portal was made in.")
                                  .arg(QFileInfo(farPath).fileName()));
            return;
        }
        m_view->scrollToPage(target);
        return;
    }
    m_view->setNotice(tr("No portal on this page."));
}

void MainWindow::redactSelection() {
    if (!m_doc->isOpen()) {
        return;
    }
    const QString text = m_view->selectedText().simplified();
    const auto [page, box] = m_view->selectionBounds();
    if (page < 0 || text.isEmpty()) {
        m_view->setNotice(tr("Select the text to remove first, on one page."));
        return;
    }
    if (!m_doc->data().isEmpty()) {
        // The document arrived through the elevation helper and exists here
        // only as bytes; there is no readable path for qpdf to work from.
        m_view->setNotice(tr("A document opened with elevated permission cannot be redacted."));
        return;
    }

    const QFileInfo source(m_doc->path());
    const QString suggested = source.absolutePath() + QLatin1Char('/') + source.completeBaseName() +
                              tr("-redacted") + QStringLiteral(".pdf");
    const QString destination = QFileDialog::getSaveFileName(
        this, tr("Save redacted copy as"), suggested, tr("PDF documents (*.pdf)"));
    if (destination.isEmpty()) {
        return;
    }

    // The view works from the top down and PDF from the bottom up.
    const QSizeF size = m_doc->pageSize(page);
    const QRectF area(box.x(), size.height() - box.y() - box.height(), box.width(), box.height());

    const Redact::Result result = Redact::run(m_doc->path(), destination, page, area, text);
    m_view->setNotice(
        result.ok ? tr("Redacted copy written to %1.").arg(QFileInfo(destination).fileName())
                  : result.error);
}

void MainWindow::showPortals() {
    auto *list = new QListWidget;
    list->setFrameShape(QFrame::NoFrame);
    list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setAutoFillBackground(false);
    list->viewport()->setAutoFillBackground(false);
    list->setBackgroundRole(QPalette::NoRole);
    list->viewport()->setBackgroundRole(QPalette::NoRole);

    if (m_portals.isEmpty()) {
        auto *empty = new QLabel(tr("No portals yet. Ctrl+M marks one end, then the other."));
        empty->setWordWrap(true);
        delete list;
        m_overlay->present(tr("Portals"), empty);
        return;
    }

    for (const Portal &portal : m_portals) {
        const QString text = tr("%1 p%2  \u2194  %3 p%4")
                                 .arg(QFileInfo(portal.a.path).fileName())
                                 .arg(portal.a.page + 1)
                                 .arg(QFileInfo(portal.b.path).fileName())
                                 .arg(portal.b.page + 1);
        auto *item = new QListWidgetItem(text, list);
        item->setData(Qt::UserRole, portal.a.path);
        item->setData(Qt::UserRole + 1, portal.a.page);
    }

    int rows = 0;
    for (int i = 0; i < list->count(); ++i) {
        rows += list->sizeHintForRow(i);
    }
    list->setFixedHeight(rows + 2);
    list->setCurrentRow(0);

    const auto go = [this](QListWidgetItem *item) {
        const QString path = item->data(Qt::UserRole).toString();
        const int page = item->data(Qt::UserRole + 1).toInt();
        m_overlay->dismiss();
        if (QFileInfo::exists(path)) {
            openPath(path);
            m_view->scrollToPage(page);
        }
    };
    connect(list, &QListWidget::itemActivated, this, go);
    connect(list, &QListWidget::itemClicked, this, go);

    m_overlay->present(tr("Portals"), list);
    list->setFocus(Qt::OtherFocusReason);
}

void MainWindow::chooseComparison() {
    if (!m_doc->isOpen()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, tr("Compare with"),
                                                      QFileInfo(m_doc->path()).absolutePath(),
                                                      tr("PDF documents (*.pdf)"));
    if (!path.isEmpty()) {
        enterCompare(path);
    }
}

void MainWindow::enterCompare(const QString &path) {
    if (isComparing() || !m_doc->isOpen()) {
        return;
    }

    m_compareDoc = std::make_unique<Document>();
    if (m_compareDoc->openPath(path) != LoadStatus::Ok) {
        m_compareDoc.reset();
        m_view->setNotice(
            tr("%1 could not be opened for comparison.").arg(QFileInfo(path).fileName()));
        return;
    }

    // The previews step aside for a comparison. Two documents side by side want
    // the whole row, and a strip that could only ever preview one of them would
    // be telling half a story.
    if (m_thumbs) {
        m_thumbs->hide();
    }
    // The page view carries a stretch of 4 so the previews beside it take about
    // a fifth. With the previews gone and a second document arriving, the two
    // documents want equal halves.
    if (auto *row = qobject_cast<QHBoxLayout *>(m_viewRow->layout())) {
        row->setStretch(row->indexOf(m_view), 1);
    }

    m_compareView = new PageView(m_viewRow);
    m_compareView->setDocument(m_compareDoc.get());
    m_compareView->setNightMode(m_view->isNightMode());
    qobject_cast<QHBoxLayout *>(m_viewRow->layout())->addWidget(m_compareView, 1);

    // Scroll-locked both ways. The guard stops the two from driving each other
    // in a loop, which is what a naive two-way binding does.
    auto *lock = new bool(false);
    m_compareView->connect(m_compareView, &QObject::destroyed, [lock] { delete lock; });
    const auto bind = [lock](PageView *from, PageView *to) {
        QObject::connect(from->verticalScrollBar(), &QScrollBar::valueChanged, to, [=](int value) {
            if (*lock) {
                return;
            }
            *lock = true;
            to->verticalScrollBar()->setValue(value);
            *lock = false;
        });
    };
    bind(m_view, m_compareView);
    bind(m_compareView, m_view);

    computeDiff();
    updateTitle();
}

void MainWindow::leaveCompare() {
    if (m_thumbs && m_thumbsAction) {
        m_thumbs->setVisible(m_thumbsAction->isChecked() && !m_view->isPresenting());
    }
    if (auto *row = qobject_cast<QHBoxLayout *>(m_viewRow->layout())) {
        row->setStretch(row->indexOf(m_view), kViewStretch);
    }
    if (!isComparing()) {
        return;
    }
    m_view->clearDiffBands();
    delete m_compareView;
    m_compareView = nullptr;
    m_compareDoc.reset();
    updateTitle();
}

QVector<QPair<double, double>> MainWindow::diffBandsFor(int page) const {
    // Rendered small on purpose. This reports that a region changed, not what
    // the change means, and says so rather than implying more precision than a
    // pixel comparison has — MZ.md §9.
    constexpr double kDiffScale = 0.35;
    constexpr int kBandRows = 6;
    constexpr int kChannelSlack = 24;

    if (!m_compareDoc) {
        return {};
    }
    // A page one document has and the other does not is entirely a difference.
    if (page >= m_compareDoc->pageCount() || page >= m_doc->pageCount()) {
        return {{0.0, 1.0}};
    }

    const QImage left = m_doc->renderPage(page, kDiffScale, Poppler::Page::Rotate0)
                            .convertToFormat(QImage::Format_RGB32);
    const QImage right = m_compareDoc->renderPage(page, kDiffScale, Poppler::Page::Rotate0)
                             .convertToFormat(QImage::Format_RGB32);
    if (left.isNull() || right.isNull()) {
        // A render that was refused is not evidence the pages match.
        return {{0.0, 1.0}};
    }

    QVector<QPair<double, double>> bands;
    const int height = qMin(left.height(), right.height());
    const int width = qMin(left.width(), right.width());
    bool open = false;
    double start = 0.0;

    for (int y = 0; y < height; y += kBandRows) {
        bool differs = left.height() != right.height() || left.width() != right.width();
        for (int row = y; row < qMin(y + kBandRows, height) && !differs; ++row) {
            const auto *a = reinterpret_cast<const QRgb *>(left.scanLine(row));
            const auto *b = reinterpret_cast<const QRgb *>(right.scanLine(row));
            for (int x = 0; x < width; ++x) {
                if (qAbs(qRed(a[x]) - qRed(b[x])) > kChannelSlack ||
                    qAbs(qGreen(a[x]) - qGreen(b[x])) > kChannelSlack ||
                    qAbs(qBlue(a[x]) - qBlue(b[x])) > kChannelSlack) {
                    differs = true;
                    break;
                }
            }
        }
        const double top = double(y) / height;
        if (differs && !open) {
            open = true;
            start = top;
        } else if (!differs && open) {
            open = false;
            bands.append({start, top});
        }
    }
    if (open) {
        bands.append({start, 1.0});
    }
    return bands;
}

void MainWindow::computeDiff() {
    if (!isComparing()) {
        return;
    }
    // Nothing is rendered here. Each view asks for a page's marks the first
    // time it paints that page, so entering a comparison is immediate however
    // long the documents are.
    const auto provider = [this](int page) { return diffBandsFor(page); };
    m_view->setDiffProvider(provider);
    m_compareView->setDiffProvider(provider);
}

void MainWindow::setPresenting(bool on) {
    // The previews are chrome, and presentation mode is the one place where
    // nothing but the page belongs on screen.
    if (m_thumbs) {
        m_thumbs->setVisible(!on && m_thumbsAction && m_thumbsAction->isChecked());
    }
    if (m_view->isPresenting() == on) {
        return;
    }
    if (on && !m_doc->isOpen()) {
        return;
    }

    if (on) {
        // Anything drawn over the page belongs to reading, not to presenting.
        if (m_overlay) {
            m_overlay->dismiss();
        }
        closeSearch();
    }

    m_view->setPresenting(on);
    m_toolBar->setVisible(!on);
    if (on) {
        showFullScreen();
    } else {
        showNormal();
    }
    m_view->setFocus(Qt::OtherFocusReason);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    // One PDF, since MERGEN holds one document. A folder or a second file is
    // not a smaller version of the request, so it is not accepted at all.
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.size() == 1 && urls.first().isLocalFile() &&
        urls.first().toLocalFile().endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent *event) {
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.size() != 1 || !urls.first().isLocalFile()) {
        return;
    }
    event->acceptProposedAction();
    openPath(urls.first().toLocalFile());
}

void MainWindow::showCommands() {
    // Unlike the other overlays this one opens with no document: Open is an
    // action, and reaching it by name is the point.
    auto *content = new QWidget;
    auto *column = new QVBoxLayout(content);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(8);

    auto *entry = new QLineEdit(content);
    entry->setPlaceholderText(tr("Page number, text to find, or an action"));
    entry->setAccessibleName(tr("Command"));
    column->addWidget(entry);

    auto *list = new QListWidget(content);
    list->setFrameShape(QFrame::NoFrame);
    list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setAutoFillBackground(false);
    list->viewport()->setAutoFillBackground(false);
    list->setBackgroundRole(QPalette::NoRole);
    list->viewport()->setBackgroundRole(QPalette::NoRole);
    list->setFocusPolicy(Qt::NoFocus);
    column->addWidget(list);

    // What each row does, parallel to the rows themselves.
    auto *deeds = new QVector<std::function<void()>>;
    content->connect(content, &QObject::destroyed, [deeds] { delete deeds; });

    const auto rebuild = [this, list, deeds](const QString &typed) {
        list->clear();
        deeds->clear();
        const QString text = typed.trimmed();

        const auto add = [list, deeds](const QString &label, std::function<void()> deed) {
            new QListWidgetItem(label, list);
            deeds->append(std::move(deed));
        };

        // A number is a page, when the document has one by that name.
        bool isNumber = false;
        const int page = text.toInt(&isNumber);
        if (isNumber && m_doc->isOpen() && page >= 1 && page <= m_doc->pageCount()) {
            add(tr("Go to page %1").arg(page), [this, page] { m_view->scrollToPage(page - 1); });
        }

        // Actions are offered by how well they answer what was typed. A name
        // the text begins is very likely what the reader meant, so it outranks
        // the find; a name that merely contains the text does not, because a
        // word being searched for should not run something by accident.
        const auto offerActions = [&](bool prefixOnly) {
            for (QAction *action :
                 {m_openAction, m_zoomOutAction, m_zoomInAction, m_fitWidthAction, m_fitPageAction,
                  m_rotateAction, m_searchAction, m_printAction}) {
                if (!action || !action->isEnabled()) {
                    continue;
                }
                const bool starts = action->text().startsWith(text, Qt::CaseInsensitive);
                const bool holds = action->text().contains(text, Qt::CaseInsensitive);
                // With nothing typed there is nothing to rank against, so every
                // action belongs in the general pass and none in the prefix one.
                if (text.isEmpty() ? prefixOnly : (prefixOnly ? !starts : (starts || !holds))) {
                    continue;
                }
                add(action->text(), [action] { action->trigger(); });
            }
        };

        if (!text.isEmpty()) {
            offerActions(true);
        }

        // Anything else that is not a bare number is something to look for.
        if (!text.isEmpty() && !isNumber && m_doc->isOpen()) {
            add(tr("Find \u201C%1\u201D").arg(text), [this, text] {
                openSearch();
                m_searchEdit->setText(text);
                startSearch();
            });
        }

        offerActions(false);

        if (text.isEmpty() || QStringLiteral("portals").contains(text, Qt::CaseInsensitive)) {
            add(tr("Portals"), [this] { showPortals(); });
        }
        // Redaction is deliberately not offered — MZ.md §13.
        //
        // The freeze audit found four independent routes by which it reported
        // success while the selected text stayed extractable, confirmed by two
        // reviewers working separately on two different PDF engines. It tests
        // only a show operator's origin rather than its painted extent; it
        // never advances the text matrix, so it deletes far more than the bar
        // covers; it drops the line advance carried by ' and ", which slides
        // the following line underneath the bar still readable; and it computes
        // its rectangle in poppler's display space while spending it as PDF
        // user space, so on any cropped page it destroys the wrong region and
        // leaves the secret. On real documents it mostly declines outright.
        //
        // A tool that says a secret is gone when it is not is worse than no
        // tool. That is the reason §3 gives for taking on qpdf at all, and it
        // is the reason the door is shut until the filter models text layout
        // properly rather than approximating it. redact.cpp stays, and its own
        // guards were repaired in Z11a, so reopening this is a matter of
        // finishing the work rather than starting it.

        int rows = 0;
        for (int i = 0; i < list->count(); ++i) {
            rows += list->sizeHintForRow(i);
        }
        list->setFixedHeight(rows + 2);
        if (list->count() > 0) {
            list->setCurrentRow(0);
        }
        // The panel is already up when the reader types, so it has to keep
        // pace with a list that grows and shrinks under them.
        if (m_overlay->isPresented()) {
            m_overlay->relayout();
        }
    };

    const auto run = [this, list, deeds] {
        const int row = list->currentRow();
        if (row < 0 || row >= deeds->size()) {
            return;
        }
        // Copied before the overlay dismisses, which destroys the row it lives on.
        const std::function<void()> deed = deeds->at(row);
        m_overlay->dismiss();
        deed();
    };

    connect(entry, &QLineEdit::textChanged, this, rebuild);
    connect(entry, &QLineEdit::returnPressed, this, run);
    connect(list, &QListWidget::itemClicked, this, [run](QListWidgetItem *) { run(); });

    // Up and Down belong to the list while the reader is typing into the field.
    entry->installEventFilter(this);
    entry->setProperty("mergenCommandList", QVariant::fromValue(static_cast<QObject *>(list)));

    rebuild(QString());
    m_overlay->present(tr("Command"), content);
    entry->setFocus(Qt::OtherFocusReason);
}

void MainWindow::showOutline() {
    if (!m_doc || !m_doc->isOpen()) {
        return;
    }

    const QVector<OutlineEntry> entries = m_doc->outline();

    if (entries.isEmpty()) {
        // Saying so beats opening an empty box: plenty of documents simply have
        // no table of contents, and that is not a failure.
        auto *empty = new QLabel(tr("This document has no table of contents."));
        empty->setWordWrap(true);
        m_overlay->present(tr("Outline"), empty);
        return;
    }

    auto *list = new QListWidget;
    list->setFrameShape(QFrame::NoFrame);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    // The overlay already scrolls. A list that scrolls inside a surface that
    // scrolls gives the reader two wheels for one list, so this one is sized to
    // its contents below and never grows bars of its own.
    list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // The panel paints the background; a view filled with Base draws a second,
    // paler rectangle on top of it.
    list->setAutoFillBackground(false);
    list->viewport()->setAutoFillBackground(false);
    list->setBackgroundRole(QPalette::NoRole);
    list->viewport()->setBackgroundRole(QPalette::NoRole);

    for (const OutlineEntry &entry : entries) {
        // Indentation carries the nesting: a transient list a reader arrows
        // through does not want branches to be expanded first.
        auto *item =
            new QListWidgetItem(QString(entry.depth * 4, QLatin1Char(' ')) + entry.title, list);
        item->setData(Qt::UserRole, entry.page);
        if (entry.page < 0) {
            // Points at another file or a URL. Listed, and inert.
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        }
    }

    // Sized to every row, since it will not scroll itself.
    int rows = 0;
    for (int i = 0; i < list->count(); ++i) {
        rows += list->sizeHintForRow(i);
    }
    list->setFixedHeight(rows + 2);
    list->setMinimumWidth(list->sizeHintForColumn(0) + 2);

    const auto jump = [this](QListWidgetItem *item) {
        const int page = item->data(Qt::UserRole).toInt();
        if (page < 0) {
            return;
        }
        m_view->scrollToPage(page);
        m_overlay->dismiss();
    };
    connect(list, &QListWidget::itemActivated, this, jump);
    connect(list, &QListWidget::itemClicked, this, jump);

    // Land on the section the reader is already in, so Enter alone is a
    // sensible thing to press.
    const int current = m_view->currentPage();
    int best = -1;
    for (int i = 0; i < entries.size(); ++i) {
        const int page = entries.at(i).page;
        if (page >= 0 && page <= current) {
            best = i;
        }
    }
    if (best >= 0) {
        list->setCurrentRow(best);
    }

    m_overlay->present(tr("Outline"), list);
    list->setFocus(Qt::OtherFocusReason);
}

void MainWindow::peekPage(int page) {
    if (!m_doc || !m_doc->isOpen() || page < 0 || page >= m_doc->pageCount()) {
        return;
    }

    // Sized against the window rather than the page: a peek is a glance, and
    // the reader is still holding the mouse down.
    const QSizeF size = m_doc->pageSize(page);
    if (!size.isValid() || size.height() <= 0) {
        return;
    }
    const double target = height() * 0.45;
    // At the screen's density, for the same reason the page view renders at it.
    const double ratio = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
    const QImage image =
        m_doc->renderPage(page, target * ratio / size.height(), m_view->rotation());
    if (image.isNull()) {
        return;
    }

    auto *label = new QLabel;
    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(ratio);
    label->setPixmap(pixmap);
    label->setAlignment(Qt::AlignCenter);

    m_overlay->present(tr("Page %1").arg(page + 1), label);
}

void MainWindow::showProperties() {
    if (!m_doc || !m_doc->isOpen()) {
        return;
    }

    const DocumentProperties props = m_doc->properties();

    auto *content = new QWidget;
    auto *grid = new QGridLayout(content);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(6);

    int line = 0;
    for (const Property &property : props.rows) {
        auto *name = new QLabel(property.name, content);
        // The same dimmed role the page view already uses for its own quiet
        // text, so the two agree without a colour being named here.
        QPalette dim = name->palette();
        dim.setColor(QPalette::WindowText, dim.color(QPalette::PlaceholderText));
        name->setPalette(dim);
        name->setAlignment(Qt::AlignRight | Qt::AlignTop);

        auto *value = new QLabel(property.value, content);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);

        grid->addWidget(name, line, 0);
        grid->addWidget(value, line, 1);
        ++line;
    }

    for (const QString &warning : props.warnings) {
        auto *label = new QLabel(warning, content);
        label->setWordWrap(true);
        QFont bold = label->font();
        bold.setBold(true);
        label->setFont(bold);
        grid->addWidget(label, line, 0, 1, 2);
        ++line;
    }

    grid->setColumnStretch(1, 1);
    m_overlay->present(tr("Document properties"), content);
}

void MainWindow::printDocument() {
    if (!m_doc->isOpen()) {
        return;
    }

    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(QFileInfo(m_doc->path()).fileName());
    printer.setFromTo(1, m_doc->pageCount());

    QPrintDialog dialog(&printer, this);
    dialog.setOption(QAbstractPrintDialog::PrintPageRange, true);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    int from = printer.fromPage();
    int to = printer.toPage();

    // printRange() was never read, so "Current page" and "Selection" both fell
    // through to the all-pages branch and printed the whole document.
    switch (printer.printRange()) {
    case QPrinter::CurrentPage:
        from = m_view->currentPage() + 1;
        to = from;
        break;
    case QPrinter::PageRange:
        break;
    case QPrinter::AllPages:
    case QPrinter::Selection:
    default:
        from = 1;
        to = m_doc->pageCount();
        break;
    }

    if (from < 1) { // The dialog leaves the range at zero for "all".
        from = 1;
        to = m_doc->pageCount();
    }
    // Both ends, not just the far one: an out-of-range start used to emit a
    // single blank sheet and say nothing.
    from = qBound(1, from, m_doc->pageCount());
    to = qBound(from, to, m_doc->pageCount());

    // A document that only root could read must not be written back out where
    // anyone can read it. Printing to a *printer* is fine — the reader
    // authenticated and the bytes go to paper — but printing to a file leaves
    // an unprivileged copy on disk that outlives the authorisation entirely.
    // The same reasoning refuses redaction on such a document (§13).
    if (!m_doc->data().isEmpty() && !printer.outputFileName().isEmpty()) {
        m_view->setNotice(
            tr("A document opened with elevated permission cannot be printed to a file."));
        return;
    }

    // Printing to a file must not land on the document being read. Redaction
    // is held to this in §8; printing was not.
    if (printer.outputFormat() == QPrinter::PdfFormat && !printer.outputFileName().isEmpty()) {
        struct stat out{};
        struct stat src{};
        if (::stat(printer.outputFileName().toLocal8Bit().constData(), &out) == 0 &&
            ::stat(m_doc->path().toLocal8Bit().constData(), &src) == 0 &&
            out.st_dev == src.st_dev && out.st_ino == src.st_ino) {
            m_view->setNotice(tr("That is the document you are reading. Choose another name."));
            return;
        }
    }

    // The printer's own resolution, not the screen cache — but capped, because
    // a full A4 at 1200 dpi is a 550 MB image and a spool file to match, and
    // nothing on paper is better for it.
    const double dpi = qMin<double>(printer.resolution(), kPrintDpiCap);
    const double scale = dpi / 72.0;
    const Poppler::Page::Rotation rotation = m_view->rotation();

    QPainter painter;
    if (!painter.begin(&printer)) {
        return;
    }

    // A thousand pages at print resolution is minutes of rendering, and the
    // window was simply frozen for all of it — measured at 346 s with no
    // repaint and no way out. The dialog gives it back: progress while it
    // works, and a cancel that is actually honoured between pages.
    m_printing = true;
    const QScopeGuard printingDone([this] { m_printing = false; });

    QProgressDialog progress(tr("Printing…"), tr("Cancel"), from, to + 1, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setWindowTitle(QFileInfo(m_doc->path()).fileName());
    // Not before there is something to wait for: a two-page print should never
    // flash a dialog.
    progress.setMinimumDuration(600);
    bool cancelled = false;

    for (int page = from; page <= to; ++page) {
        progress.setValue(page);
        // Rendering is synchronous, so the dialog only paints and only sees the
        // Cancel click if the loop lets the event queue run.
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents | QEventLoop::AllEvents);
        if (progress.wasCanceled()) {
            cancelled = true;
            break;
        }
        if (page != from && !printer.newPage()) {
            break;
        }
        const QImage image = m_doc->renderPage(page - 1, scale, rotation);
        if (image.isNull()) {
            continue;
        }
        // Fit the page to the paper, keeping its proportions.
        const QRect paper = painter.viewport();
        QSize target = image.size();
        target.scale(paper.size(), Qt::KeepAspectRatio);
        const QRect placed(paper.x() + (paper.width() - target.width()) / 2,
                           paper.y() + (paper.height() - target.height()) / 2, target.width(),
                           target.height());
        painter.drawImage(placed, image);
    }
    progress.setValue(to + 1);

    if (cancelled) {
        // Whatever reached the spooler stays there — a printer cannot unprint —
        // but the reader is told rather than left guessing at a short stack.
        painter.end();
        printer.abort();
        m_view->setNotice(tr("Printing stopped. Pages already sent will still print."));
        return;
    }
    painter.end();
}

// --- About -----------------------------------------------------------------

void MainWindow::showAbout() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("About MERGEN"));

    auto *column = new QVBoxLayout(&dialog);
    column->setContentsMargins(28, 24, 28, 20);
    column->setSpacing(0);

    // The family's About layout: the mark, the maker, version, release date,
    // source address, then the licence in full. Addresses are selectable and
    // never clickable — that is the rule the whole family keeps, and it is why
    // nothing here is a QLabel with rich text or an open-external-link flag.
    const QColor ink = dialog.palette().color(QPalette::WindowText);
    QColor quiet = ink;
    quiet.setAlpha(140);

    // The mark itself, above the wordmark.
    auto *badge = new QLabel(&dialog);
    const int badgeSize = 72;
    QPixmap art = applicationIcon().pixmap(QSize(badgeSize, badgeSize), dialog.devicePixelRatioF());
    art.setDevicePixelRatio(dialog.devicePixelRatioF());
    badge->setPixmap(art);
    badge->setAlignment(Qt::AlignHCenter);
    column->addWidget(badge);
    column->addSpacing(10);

    auto *mark = new QLabel(QStringLiteral("MERGEN"), &dialog);
    QFont markFont = mark->font();
    markFont.setPointSizeF(markFont.pointSizeF() * 2.6);
    markFont.setWeight(QFont::Light);
    markFont.setLetterSpacing(QFont::AbsoluteSpacing, 6.0);
    mark->setFont(markFont);
    mark->setTextFormat(Qt::PlainText);
    mark->setAlignment(Qt::AlignHCenter);
    column->addWidget(mark);

    // Bilingual, as the family's pages are.
    auto *subtitle = new QLabel(tr("A minimal PDF viewer  ·  Sade bir PDF okuyucu"), &dialog);
    QFont subtitleFont = subtitle->font();
    subtitleFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    subtitle->setFont(subtitleFont);
    subtitle->setTextFormat(Qt::PlainText);
    subtitle->setAlignment(Qt::AlignHCenter);
    {
        QPalette p = subtitle->palette();
        p.setColor(QPalette::WindowText, quiet);
        subtitle->setPalette(p);
    }
    column->addSpacing(2);
    column->addWidget(subtitle);
    column->addSpacing(18);

    auto *rule = new QFrame(&dialog);
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Plain);
    rule->setFixedHeight(1);
    column->addWidget(rule);
    column->addSpacing(16);

    // One grid, so the values line up rather than sitting in a paragraph.
    auto *facts = new QGridLayout;
    facts->setHorizontalSpacing(18);
    facts->setVerticalSpacing(6);
    facts->setColumnStretch(1, 1);
    const QVector<QPair<QString, QString>> rows = {
        {tr("Version"), QStringLiteral(MERGEN_VERSION)},
        {tr("Released"), QStringLiteral(MERGEN_RELEASE_DATE)},
        {tr("Made by"), QStringLiteral("MEGAS")},
        {tr("Source"), QStringLiteral("github.com/sudo-megas/MERGEN")},
        {tr("Licence"), tr("GNU General Public License, version 3")},
    };
    for (int i = 0; i < rows.size(); ++i) {
        auto *key = new QLabel(rows.at(i).first, &dialog);
        key->setTextFormat(Qt::PlainText);
        {
            QPalette p = key->palette();
            p.setColor(QPalette::WindowText, quiet);
            key->setPalette(p);
        }
        auto *value = new QLabel(rows.at(i).second, &dialog);
        value->setTextFormat(Qt::PlainText);
        // Selectable so the address can be copied; never a link.
        value->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        facts->addWidget(key, i, 0, Qt::AlignRight | Qt::AlignVCenter);
        facts->addWidget(value, i, 1, Qt::AlignLeft | Qt::AlignVCenter);
    }
    column->addLayout(facts);
    column->addSpacing(18);

    auto *licence = new QPlainTextEdit(&dialog);
    licence->setReadOnly(true);
    licence->setPlainText(QString::fromUtf8(kLicenseText));
    licence->setLineWrapMode(QPlainTextEdit::NoWrap);
    licence->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    licence->setMinimumSize(620, 300);
    // Without this the box asks for the width of the longest line it holds and
    // the dialog obliges, which is where the sliver past the right edge came
    // from — the window ended up wider than anything drawn into it.
    licence->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    column->addWidget(licence, 1);
    column->addSpacing(14);

    auto *sign = new QLabel(tr("Built with Reason and Passion"), &dialog);
    sign->setTextFormat(Qt::PlainText);
    sign->setAlignment(Qt::AlignHCenter);
    QFont signFont = sign->font();
    signFont.setItalic(true);
    sign->setFont(signFont);
    {
        QPalette p = sign->palette();
        p.setColor(QPalette::WindowText, quiet);
        sign->setPalette(p);
    }
    column->addWidget(sign);
    column->addSpacing(12);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    column->addWidget(buttons);

    dialog.exec();
}

// --- Search ----------------------------------------------------------------

void MainWindow::buildSearchBar() {
    m_searchBar = new QWidget(centralWidget() ? centralWidget() : this);
    auto *row = new QHBoxLayout(m_searchBar);
    row->setContentsMargins(6, 4, 6, 4);
    row->setSpacing(6);

    m_searchEdit = new QLineEdit(m_searchBar);
    m_searchEdit->setPlaceholderText(tr("Find in document"));
    m_searchEdit->installEventFilter(this);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, [this] {
        if (m_searchEdit->text().isEmpty()) {
            return;
        }
        // Re-running the same needle means "next hit", not "search again".
        if (m_view->searchHitCount() > 0 && !m_searchWorker) {
            m_view->nextSearchHit();
        } else {
            startSearch();
        }
    });
    row->addWidget(m_searchEdit, 1);

    // Thin, and only present while a pass is running.
    m_searchProgress = new QProgressBar(m_searchBar);
    m_searchProgress->setTextVisible(false);
    m_searchProgress->setFixedHeight(4);
    m_searchProgress->setFixedWidth(120);
    m_searchProgress->hide();
    row->addWidget(m_searchProgress);

    m_searchCancel = new QPushButton(m_searchBar);
    m_searchCancel->setText(tr("Cancel"));
    m_searchCancel->setToolTip(tr("Stop the search and keep the hits found so far"));
    m_searchCancel->setFlat(true);
    m_searchCancel->hide();
    connect(m_searchCancel, &QPushButton::clicked, this, [this] {
        // The button keeps whatever was found; only Esc clears.
        cancelSearch();
        updateSearchStatus();
    });
    row->addWidget(m_searchCancel);

    m_searchStatus = new QLabel(m_searchBar);
    row->addWidget(m_searchStatus);

    m_searchBar->hide();
}

void MainWindow::openSearch() {
    if (!m_doc->isOpen()) {
        return;
    }
    m_searchBar->show();
    m_searchEdit->setFocus();
    m_searchEdit->selectAll();
}

void MainWindow::closeSearch() {
    cancelSearch();
    m_view->clearSearchHits();
    m_searchBar->hide();
    m_searchProgress->hide();
    m_searchCancel->hide();
    m_view->setFocus();
}

void MainWindow::startSearch() {
    cancelSearch();
    m_view->clearSearchHits();

    const QString needle = m_searchEdit->text();
    if (needle.isEmpty() || !m_doc->isOpen()) {
        updateSearchStatus();
        return;
    }

    if (!m_searchThread) {
        m_searchThread = new QThread(this);
        m_searchThread->start();
    }

    auto *worker = new SearchWorker(m_doc->path(), m_doc->data(), needle);
    worker->moveToThread(m_searchThread);
    connect(worker, &SearchWorker::hitFound, this, &MainWindow::onSearchHit);
    connect(worker, &SearchWorker::progress, this, &MainWindow::onSearchProgress);
    connect(worker, &SearchWorker::done, this, &MainWindow::onSearchDone);
    // Receiver is the worker itself, so this survives the disconnect in
    // cancelSearch() and the worker still cleans itself up.
    connect(worker, &SearchWorker::done, worker, &QObject::deleteLater);
    m_searchWorker = worker;

    m_searchProgress->setRange(0, 100);
    m_searchProgress->setValue(0);
    m_searchProgress->show();
    m_searchCancel->show();
    updateSearchStatus();

    QMetaObject::invokeMethod(worker, "run", Qt::QueuedConnection);
}

void MainWindow::cancelSearch() {
    if (!m_searchWorker) {
        return;
    }
    // Detach first: hits already queued from the old pass must not land in the
    // new one's result list.
    disconnect(m_searchWorker, nullptr, this, nullptr);
    m_searchWorker->cancel();
    m_searchWorker = nullptr;
    m_searchProgress->hide();
    m_searchCancel->hide();
}

// Disconnecting a worker does not discard the signals it has already queued
// across the thread boundary, so a replaced pass can still deliver hits after
// the new one has cleared the list. Everything the worker sends is therefore
// checked against the pass that is actually current.
void MainWindow::onSearchHit(int page, const QRectF &rect) {
    if (sender() != m_searchWorker) {
        return;
    }
    m_view->addSearchHit(page, rect);
}

void MainWindow::onSearchProgress(int page, int total) {
    if (sender() != m_searchWorker) {
        return;
    }
    if (total > 0) {
        m_searchProgress->setValue(page * 100 / total);
    }
    updateSearchStatus();
}

void MainWindow::onSearchDone(bool cancelled) {
    Q_UNUSED(cancelled);
    if (sender() != m_searchWorker) {
        return;
    }
    m_searchWorker = nullptr;
    m_searchProgress->hide();
    m_searchCancel->hide();
    updateSearchStatus();
}

void MainWindow::updateSearchStatus() {
    const int count = m_view->searchHitCount();
    if (count == 0) {
        m_searchStatus->setText(m_searchWorker ? tr("Searching…") : tr("No hits"));
        return;
    }
    const int current = m_view->currentSearchHit();
    m_searchStatus->setText(m_searchWorker ? tr("%1 of %2 so far").arg(current + 1).arg(count)
                                           : tr("%1 of %2").arg(current + 1).arg(count));
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    // The command field keeps focus while Up and Down move its list, so the
    // reader never has to leave what they are typing to choose a row.
    if (event->type() == QEvent::KeyPress) {
        const QVariant carried = watched->property("mergenCommandList");
        if (carried.isValid()) {
            auto *list = qobject_cast<QListWidget *>(carried.value<QObject *>());
            auto *key = static_cast<QKeyEvent *>(event);
            if (list && list->count() > 0 &&
                (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up)) {
                const int step = key->key() == Qt::Key_Down ? 1 : -1;
                const int row = (list->currentRow() + step + list->count()) % list->count();
                list->setCurrentRow(row);
                return true;
            }
        }
    }
    if (watched == m_searchEdit && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            closeSearch();
            return true;
        }
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            (key->modifiers() & Qt::ShiftModifier)) {
            m_view->previousSearchHit();
            updateSearchStatus();
            return true;
        }
        if (key->key() == Qt::Key_Down) {
            m_view->nextSearchHit();
            updateSearchStatus();
            return true;
        }
        if (key->key() == Qt::Key_Up) {
            m_view->previousSearchHit();
            updateSearchStatus();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

QString MainWindow::recentFilePath() {
    QString base = QString::fromLocal8Bit(qgetenv("XDG_STATE_HOME"));
    if (base.isEmpty()) {
        base = QDir::homePath() + QStringLiteral("/.local/state");
    }
    return base + QStringLiteral("/mergen/recent.toml");
}

void MainWindow::loadRecent() {
    m_recent.clear();

    QFile file(recentFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return; // A missing state file is never an error the user sees.
    }
    QString text = QString::fromUtf8(file.readAll());
    file.close();

    // Comments are not values. The bracket scan below reads the whole file, so
    // a "#" line containing a bracket could otherwise inject an entry.
    {
        QStringList kept;
        const QStringList lines = text.split(QLatin1Char('\n'));
        kept.reserve(lines.size());
        for (const QString &line : lines) {
            const QString trimmed = line.trimmed();
            if (!trimmed.startsWith(QLatin1Char('#'))) {
                kept.append(line);
            }
        }
        text = kept.join(QLatin1Char('\n'));
    }

    // Deliberately trivial: MERGEN writes this file and reads only what it
    // writes. Anything unexpected yields an empty list, which the next open
    // overwrites.
    const int open = text.indexOf(QLatin1Char('['));
    const int close = text.lastIndexOf(QLatin1Char(']'));
    if (open < 0 || close < open) {
        return;
    }
    const QString body = text.mid(open + 1, close - open - 1);

    int i = 0;
    while (i < body.size() && m_recent.size() < kRecentLimit) {
        if (body.at(i) != QLatin1Char('"')) {
            ++i;
            continue;
        }
        QString raw;
        ++i;
        while (i < body.size()) {
            const QChar c = body.at(i);
            if (c == QLatin1Char('\\') && i + 1 < body.size()) {
                raw += c;
                raw += body.at(++i);
                ++i;
                continue;
            }
            if (c == QLatin1Char('"')) {
                ++i;
                break;
            }
            raw += c;
            ++i;
        }
        const QString path = tomlUnescape(raw);
        if (!path.isEmpty() && !m_recent.contains(path)) {
            m_recent.append(path);
        }
    }
}

void MainWindow::saveRecent() const {
    const QString target = recentFilePath();
    QDir().mkpath(QFileInfo(target).absolutePath());

    QString text = QStringLiteral("# MERGEN recent files\nrecent = [\n");
    for (const QString &path : m_recent) {
        text += QStringLiteral("    \"%1\",\n").arg(tomlEscape(path));
    }
    text += QStringLiteral("]\n");

    // Temp file, flush, rename — MX.md §7.
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }
    file.write(text.toUtf8());
    file.commit();
}

void MainWindow::pushRecent(const QString &path) {
    if (path.isEmpty()) {
        return;
    }
    m_recent.removeAll(path);
    m_recent.prepend(path);

    // Paths that have gone away are dropped the next time the file is written.
    for (int i = m_recent.size() - 1; i > 0; --i) {
        if (!QFileInfo::exists(m_recent.at(i))) {
            m_recent.removeAt(i);
        }
    }
    while (m_recent.size() > kRecentLimit) {
        m_recent.removeLast();
    }

    saveRecent();
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu() {
    if (!m_recentMenu) {
        return;
    }
    m_recentMenu->clear();

    if (m_recent.isEmpty()) {
        QAction *empty = m_recentMenu->addAction(tr("No recent files"));
        empty->setEnabled(false);
        return;
    }

    for (const QString &path : m_recent) {
        QAction *entry = m_recentMenu->addAction(QFileInfo(path).fileName());
        entry->setToolTip(path);
        // Greyed, but still listed: it was recent, it is simply not there now.
        entry->setEnabled(QFileInfo::exists(path));
        connect(entry, &QAction::triggered, this, [this, path] { openPath(path); });
    }
}

} // namespace mergen
