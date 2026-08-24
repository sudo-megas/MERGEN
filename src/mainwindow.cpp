// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "mainwindow.h"
#include "document.h"
#include "pageview.h"

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
#include <QEventLoop>
#include <QProcess>
#include <QSaveFile>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>

namespace mergen {
namespace {

// Font Awesome 5 code points as patched into CaskaydiaCove Nerd Font. Set as
// action text in a glyph font, so MERGEN needs no icon theme and ships no
// image assets — see MX.md §5.
constexpr char16_t kGlyphOpen = u'';
constexpr char16_t kGlyphZoomOut = u'';
constexpr char16_t kGlyphZoomIn = u'';
constexpr char16_t kGlyphFitWidth = u'';
constexpr char16_t kGlyphFitPage = u'';
constexpr char16_t kGlyphRotate = u'';
constexpr char16_t kGlyphSearch = u'';
constexpr char16_t kGlyphPrint = u'';

constexpr int kRecentLimit = 10;

QString glyph(char16_t code) {
    return QString(QChar(code));
}

// One string: compositor-drawn decorations have no separate zones to fill.
QString titleFor(const QString &fileName) {
    if (fileName.isEmpty()) {
        return QStringLiteral(R"(MERGEN ///— —\\\ MEGAS)");
    }
    return QStringLiteral(R"(MERGEN ///— %1 —\\\ MEGAS)").arg(fileName);
}

/// The toolbar font: the Nerd Font when it is installed, otherwise whatever Qt
/// hands us. A missing font costs the glyphs, not the toolbar.
QFont glyphFont() {
    QFont font = QApplication::font();
    const QStringList families = QFontDatabase::families();
    for (const QString &candidate :
         {QStringLiteral("CaskaydiaCove Nerd Font"), QStringLiteral("CaskaydiaCove NF"),
          QStringLiteral("CaskaydiaMono Nerd Font")}) {
        if (families.contains(candidate)) {
            font.setFamily(candidate);
            break;
        }
    }
    return font;
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

    m_view = new PageView(this);
    setCentralWidget(m_view);

    m_watcher = new QFileSystemWatcher(this);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &) {
        // Never reload behind the reader's back — offer, and wait to be asked.
        m_view->setNotice(tr("This file changed on disk. Click to reload."));
    });
    connect(m_view, &PageView::noticeClicked, this, &MainWindow::reloadDocument);
    connect(m_view, &PageView::pageChanged, this, &MainWindow::onPageChanged);

    loadRecent();
    buildToolBar();
    updateTitle();
    onPageChanged(-1);
}

MainWindow::~MainWindow() = default;

QToolButton *MainWindow::addGlyphAction(QAction *action, const QString &glyphText,
                                        const QString &label) {
    action->setText(glyphText + QStringLiteral("  ") + label);
    auto *button = new QToolButton(m_toolBar);
    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    m_toolBar->addWidget(button);
    return button;
}

void MainWindow::buildToolBar() {
    m_toolBar = addToolBar(QStringLiteral("MERGEN"));
    m_toolBar->setMovable(false);
    m_toolBar->setFloatable(false);
    m_toolBar->setContextMenuPolicy(Qt::PreventContextMenu);
    m_toolBar->setFont(glyphFont());

    m_openAction = new QAction(this);
    m_openAction->setShortcut(QKeySequence::Open);
    connect(m_openAction, &QAction::triggered, this, &MainWindow::chooseFile);
    QToolButton *openButton = addGlyphAction(m_openAction, glyph(kGlyphOpen), tr("Open"));
    m_recentMenu = new QMenu(openButton);
    openButton->setMenu(m_recentMenu);
    openButton->setPopupMode(QToolButton::MenuButtonPopup);
    rebuildRecentMenu();

    m_zoomOutAction = new QAction(this);
    m_zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    connect(m_zoomOutAction, &QAction::triggered, m_view, &PageView::zoomOut);
    addGlyphAction(m_zoomOutAction, glyph(kGlyphZoomOut), tr("Zoom out"));

    m_zoomInAction = new QAction(this);
    // QKeySequence::ZoomIn is Ctrl++, which most keyboards produce as Ctrl+=.
    m_zoomInAction->setShortcuts({QKeySequence::ZoomIn, QKeySequence(QStringLiteral("Ctrl+="))});
    connect(m_zoomInAction, &QAction::triggered, m_view, &PageView::zoomIn);
    addGlyphAction(m_zoomInAction, glyph(kGlyphZoomIn), tr("Zoom in"));

    m_fitWidthAction = new QAction(this);
    connect(m_fitWidthAction, &QAction::triggered, m_view, &PageView::setFitWidth);
    addGlyphAction(m_fitWidthAction, glyph(kGlyphFitWidth), tr("Fit width"));

    m_fitPageAction = new QAction(this);
    connect(m_fitPageAction, &QAction::triggered, m_view, &PageView::setFitPage);
    addGlyphAction(m_fitPageAction, glyph(kGlyphFitPage), tr("Fit page"));

    m_rotateAction = new QAction(this);
    m_rotateAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    connect(m_rotateAction, &QAction::triggered, m_view, &PageView::rotateClockwise);
    addGlyphAction(m_rotateAction, glyph(kGlyphRotate), tr("Rotate"));

    m_searchAction = new QAction(this);
    m_searchAction->setShortcut(QKeySequence::Find);
    addGlyphAction(m_searchAction, glyph(kGlyphSearch), tr("Search"));

    m_printAction = new QAction(this);
    m_printAction->setShortcut(QKeySequence::Print);
    addGlyphAction(m_printAction, glyph(kGlyphPrint), tr("Print"));

    // Counter-clockwise rotation is a keybinding only; §5 gives the toolbar one
    // rotate button.
    m_rotateBackAction = new QAction(this);
    m_rotateBackAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+R")));
    connect(m_rotateBackAction, &QAction::triggered, m_view,
            &PageView::rotateCounterClockwise);
    addAction(m_rotateBackAction);

    auto *resetZoomAction = new QAction(this);
    resetZoomAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    connect(resetZoomAction, &QAction::triggered, m_view, &PageView::resetZoom);
    addAction(resetZoomAction);

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
    connect(m_pageEdit, &QLineEdit::returnPressed, this, &MainWindow::jumpToTypedPage);
    m_toolBar->addWidget(m_pageEdit);

    m_pageTotal = new QLabel(m_toolBar);
    m_toolBar->addWidget(m_pageTotal);
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
    const QString chosen = QFileDialog::getOpenFileName(this, tr("Open PDF"), start,
                                                        tr("PDF documents (*.pdf)"));
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
    const int count = m_doc->pageCount();
    if (!m_doc->isOpen() || count <= 0) {
        m_pageEdit->clear();
        m_pageEdit->setEnabled(false);
        m_pageTotal->setText(QStringLiteral(" / 0"));
        return;
    }
    m_pageEdit->setEnabled(true);
    static_cast<QIntValidator *>(const_cast<QValidator *>(m_pageEdit->validator()))
        ->setTop(count);
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
            QInputDialog::getText(this, tr("MERGEN"),
                                  tr("Password for %1:").arg(fileName), QLineEdit::Password,
                                  QString(), &accepted);
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
