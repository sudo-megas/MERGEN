// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QMainWindow>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

class QAction;
class QLabel;
class QProgressBar;
class QPushButton;
class QThread;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QMenu;
class QToolBar;
class QToolButton;

namespace mergen {

class Document;
class Overlay;
class PageView;
class SearchWorker;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    /// The single entry point for every way a document is opened: argv[1], the
    /// file dialog, and the recent-files dropdown all land here.
    void openPath(const QString &path);

private:
    void buildToolBar();
    /// Builds one toolbar button: the glyph becomes the action's icon and the
    /// label becomes its text. With no menu bar the tooltip is the only place
    /// a shortcut is discoverable, which makes it load-bearing here — MZ.md §6.
    /// \a keyHint names keys handled in the page view rather than registered as
    /// window-wide shortcuts, which single letters must never be.
    QToolButton *addGlyphAction(QAction *action, char16_t glyph, const QString &label,
                                const QString &keyHint = QString());

    /// Re-renders every toolbar icon against the current palette. Called when
    /// the theme changes: a pixmap tinted for the old palette is wrong.
    void applyIcons();

    /// Enables and disables toolbar actions against what is actually possible
    /// right now — no document, or zoom already at its bound.
    void updateActionStates();

    void changeEvent(QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

    void updateTitle();
    void closeDocument(const QString &message = QString());
    void showError(const QString &message);

    // Actions.
    void showProperties();
    void showOutline();
    /// Fullscreen, chrome withdrawn, one page at a time. Esc leaves.
    void setPresenting(bool on);
    /// One surface for a page number, a search term, or any action by name.
    /// What the reader typed decides which — MZ.md \ref 6.
    void showCommands();
    /// Renders one page into the overlay without going there. The reader is
    /// holding a link down; letting go puts everything back.
    void peekPage(int page);
    void printDocument();
    void showAbout();
    void chooseFile();
    void reloadDocument();
    void jumpToTypedPage();
    void onPageChanged(int index);

    // Recent files — MX.md §7.
    static QString recentFilePath();
    void loadRecent();
    void saveRecent() const;
    void pushRecent(const QString &path);
    void rebuildRecentMenu();

    // Password-protected documents — the password is never persisted and is
    // wiped from memory as soon as it has been tried.
    bool promptForPassword(const QString &fileName);

    /// Reads a file the user cannot open by running the helper under pkexec.
    /// Returns empty on refusal or failure and sets a user-facing sentence.
    QByteArray readElevated(const QString &path, QString *error) const;

    void watchDocument(const QString &path);

    // Search — MX.md §8. The pass runs on a worker thread with its own
    // document handle and reports hits as it finds them.
    void buildSearchBar();
    void openSearch();
    void closeSearch();
    void startSearch();
    /// Stops a pass in flight but keeps the hits it already produced.
    void cancelSearch();
    void onSearchHit(int page, const QRectF &rect);
    void onSearchProgress(int page, int total);
    void onSearchDone(bool cancelled);
    void updateSearchStatus();
    bool eventFilter(QObject *watched, QEvent *event) override;

    std::unique_ptr<Document> m_doc;
    PageView *m_view = nullptr;
    Overlay *m_overlay = nullptr;
    QToolBar *m_toolBar = nullptr;

    QAction *m_openAction = nullptr;
    QAction *m_zoomOutAction = nullptr;
    QAction *m_zoomInAction = nullptr;
    QAction *m_fitWidthAction = nullptr;
    QAction *m_fitPageAction = nullptr;
    QAction *m_rotateAction = nullptr;
    QAction *m_rotateBackAction = nullptr;
    QAction *m_searchAction = nullptr;
    QAction *m_printAction = nullptr;
    QAction *m_quitAction = nullptr;

    /// Every action carrying a glyph, so all icons can be re-rendered together
    /// when the palette changes.
    QVector<QPair<QAction *, char16_t>> m_glyphActions;

    QMenu *m_recentMenu = nullptr;
    QStringList m_recent;

    QLineEdit *m_pageEdit = nullptr;
    QLabel *m_pageTotal = nullptr;

    QFileSystemWatcher *m_watcher = nullptr;

    QWidget *m_searchBar = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QProgressBar *m_searchProgress = nullptr;
    QPushButton *m_searchCancel = nullptr;
    QLabel *m_searchStatus = nullptr;

    /// One thread for the life of the window; a pass is a worker moved onto
    /// it. Cancelling lets the old worker fall out of its loop before the next
    /// one starts, so there is never more than one document handle in flight.
    QThread *m_searchThread = nullptr;
    QPointer<SearchWorker> m_searchWorker;

    /// Guards against re-entering openPath from a nested event loop.
    bool m_opening = false;
};

} // namespace mergen
