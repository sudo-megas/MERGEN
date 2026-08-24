// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "mainwindow.h"
#include "document.h"
#include "control.h"
#include "iconset.h"
#include "overlay.h"
#include "pageview.h"
#include "redact.h"
#include "license.h"

#include <QAction>
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
#include <QDialogButtonBox>
#include <QPainter>
#include <QPlainTextEdit>
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
#include <QToolBar>
#include <QToolButton>
#include <QWidget>

#include <utility>

namespace mergen {
namespace {

constexpr int kRecentLimit = 10;

/// Ceiling on print rendering. Above this the image cost climbs fast and the
/// paper does not improve.
constexpr int kPrintDpiCap = 600;

/// Alpha applied to the palette's highlight colour for the two interactive
/// states. Proportions, not colours: the hue is always the reader's own.
constexpr int kHoverAlpha = 46;
constexpr int kPressedAlpha = 82;

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

        if (pressed || hovered) {
            QColor wash = palette().color(QPalette::Highlight);
            wash.setAlpha(pressed ? kPressedAlpha : kHoverAlpha);
            const qreal radius = height() * 0.18;
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(Qt::NoPen);
            painter.setBrush(wash);
            painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
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

    m_view = new PageView(m_viewRow);
    m_overlay = new Overlay(m_view);
    row->addWidget(m_view, 1);

    buildSearchBar();
    column->addWidget(m_searchBar);
    column->addWidget(m_viewRow, 1);
    setCentralWidget(central);

    m_watcher = new QFileSystemWatcher(this);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &) {
        // Never reload behind the reader's back — offer, and wait to be asked.
        m_view->setNotice(tr("This file changed on disk. Click to reload."));
    });
    connect(m_view, &PageView::noticeClicked, this, &MainWindow::reloadDocument);
    connect(m_view, &PageView::pageChanged, this, &MainWindow::onPageChanged);
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
        m_searchThread->quit();
        m_searchThread->wait(3000);
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

    m_toolBar->addSeparator();

    m_zoomOutAction = new QAction(this);
    m_zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    connect(m_zoomOutAction, &QAction::triggered, m_view, &PageView::zoomOut);
    addGlyphAction(m_zoomOutAction, glyphs::kZoomOut, tr("Zoom out"));

    m_zoomInAction = new QAction(this);
    // QKeySequence::ZoomIn is Ctrl++, which most keyboards produce as Ctrl+=.
    m_zoomInAction->setShortcuts({QKeySequence::ZoomIn, QKeySequence(QStringLiteral("Ctrl+="))});
    connect(m_zoomInAction, &QAction::triggered, m_view, &PageView::zoomIn);
    addGlyphAction(m_zoomInAction, glyphs::kZoomIn, tr("Zoom in"));

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

    m_toolBar->addSeparator();

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
    connect(nightAction, &QAction::triggered, this,
            [this] { m_view->setNightMode(!m_view->isNightMode()); });
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
    auto *spacer = new QWidget(m_toolBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toolBar->addWidget(spacer);

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

    // Zoom bounds decide whether the two zoom buttons are still live.
    connect(m_view, &PageView::zoomChanged, this, [this](double) { updateActionStates(); });

    applyIcons();
    updateActionStates();
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
    m_doc->close();
    m_view->setDocument(nullptr);
    m_view->setNotice(QString());
    m_view->setMessage(message);
    if (!m_watcher->files().isEmpty()) {
        m_watcher->removePaths(m_watcher->files());
    }
    updateTitle();
    onPageChanged(-1);
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
    pkexec.setProgram(QStringLiteral("pkexec"));
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

    if (verb == QLatin1String("open")) {
        if (argument.isEmpty()) {
            return err(QStringLiteral("open needs a path"));
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
    Portal current;
    int ends = 0;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line == QLatin1String("[[portal]]")) {
            // A portal needs both ends; a half-written one is dropped rather
            // than kept as something that goes nowhere.
            if (ends == 2) {
                m_portals.append(current);
            }
            current = Portal();
            ends = 0;
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
        PortalEnd &end = ends == 0 ? current.a : current.b;
        if (key == QLatin1String("hash")) {
            end.hash = value;
        } else if (key == QLatin1String("path")) {
            end.path = value;
        } else if (key == QLatin1String("page")) {
            end.page = value.toInt();
            ++ends;
        }
    }
    if (ends == 2) {
        m_portals.append(current);
    }
}

void MainWindow::savePortals() const {
    const QString path = portalFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
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
    file.commit();
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
    savePortals();
    m_view->setNotice(tr("Portal made. Ctrl+J follows it."));
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
        const int target = to->page;
        openPath(to->path);
        if (m_doc->isOpen()) {
            m_view->scrollToPage(target);
        }
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
    if (!isComparing()) {
        return;
    }
    m_view->clearDiffBands();
    delete m_compareView;
    m_compareView = nullptr;
    m_compareDoc.reset();
    updateTitle();
}

void MainWindow::computeDiff() {
    if (!isComparing()) {
        return;
    }
    // Rendered small on purpose. This reports that a region changed, not what
    // the change means, and says so rather than implying more precision than a
    // pixel comparison has — MZ.md §9.
    constexpr double kDiffScale = 0.35;
    constexpr int kBandRows = 6;
    constexpr int kChannelSlack = 24;

    const int pages = qMin(m_doc->pageCount(), m_compareDoc->pageCount());
    for (int page = 0; page < pages; ++page) {
        const QImage left = m_doc->renderPage(page, kDiffScale, Poppler::Page::Rotate0)
                                .convertToFormat(QImage::Format_RGB32);
        const QImage right = m_compareDoc->renderPage(page, kDiffScale, Poppler::Page::Rotate0)
                                 .convertToFormat(QImage::Format_RGB32);
        if (left.isNull() || right.isNull()) {
            continue;
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

        m_view->setDiffBands(page, bands);
        m_compareView->setDiffBands(page, bands);
    }

    // A document the other does not reach is entirely a difference.
    for (int page = pages; page < m_doc->pageCount(); ++page) {
        m_view->setDiffBands(page, {{0.0, 1.0}});
    }
    for (int page = pages; page < m_compareDoc->pageCount(); ++page) {
        m_compareView->setDiffBands(page, {{0.0, 1.0}});
    }
}

void MainWindow::setPresenting(bool on) {
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
        if (!m_view->selectedText().trimmed().isEmpty() &&
            (text.isEmpty() || QStringLiteral("redact").contains(text, Qt::CaseInsensitive))) {
            add(tr("Redact selection"), [this] { redactSelection(); });
        }

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
    const QImage image = m_doc->renderPage(page, target / size.height(), m_view->rotation());
    if (image.isNull()) {
        return;
    }

    auto *label = new QLabel;
    label->setPixmap(QPixmap::fromImage(image));
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
    if (from < 1) { // "All pages": the dialog leaves the range at zero.
        from = 1;
        to = m_doc->pageCount();
    }
    to = qMin(to, m_doc->pageCount());

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

    for (int page = from; page <= to; ++page) {
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
    painter.end();
}

// --- About -----------------------------------------------------------------

void MainWindow::showAbout() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("About MERGEN"));

    auto *column = new QVBoxLayout(&dialog);

    // Plain text throughout, and no link handling anywhere: the address is
    // there to be read and copied, never to open a browser. MX.md §4.
    auto *heading = new QLabel(&dialog);
    heading->setTextFormat(Qt::PlainText);
    heading->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    heading->setText(tr("MERGEN %1\n"
                        "A minimal PDF viewer\n\n"
                        "Made by MEGAS\n"
                        "Released %2\n"
                        "Source: github.com/sudo-megas/MERGEN\n\n"
                        "Licensed under the GNU General Public License, version 3.")
                         .arg(QStringLiteral(MERGEN_VERSION))
                         .arg(QStringLiteral(MERGEN_RELEASE_DATE)));
    column->addWidget(heading);

    auto *licence = new QPlainTextEdit(&dialog);
    licence->setReadOnly(true);
    licence->setPlainText(QString::fromUtf8(kLicenseText));
    licence->setLineWrapMode(QPlainTextEdit::NoWrap);
    licence->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    licence->setMinimumSize(640, 360);
    column->addWidget(licence, 1);

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
    const QString text = QString::fromUtf8(file.readAll());
    file.close();

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
