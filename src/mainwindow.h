// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QMainWindow>
#include <QString>

#include <memory>

class QToolBar;

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
    void updateTitle();
    void closeDocument();

    std::unique_ptr<Document> m_doc;
    PageView *m_view = nullptr;
    QToolBar *m_toolBar = nullptr;
};

} // namespace mergen
