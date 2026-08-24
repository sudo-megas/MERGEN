// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QMainWindow>
#include <QString>
#include <QStringList>

#include <memory>

class QAction;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QMenu;
class QToolBar;
class QToolButton;

namespace mergen {

class Document;
class PageView;

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
    QToolButton *addGlyphAction(QAction *action, const QString &glyph, const QString &label);

    void updateTitle();
    void closeDocument(const QString &message = QString());
    void showError(const QString &message);

    // Actions.
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

    std::unique_ptr<Document> m_doc;
    PageView *m_view = nullptr;
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

    QMenu *m_recentMenu = nullptr;
    QStringList m_recent;

    QLineEdit *m_pageEdit = nullptr;
    QLabel *m_pageTotal = nullptr;

    QFileSystemWatcher *m_watcher = nullptr;

    /// Guards against re-entering openPath from a nested event loop.
    bool m_opening = false;
};

} // namespace mergen
