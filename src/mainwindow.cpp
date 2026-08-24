// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "mainwindow.h"
#include "document.h"
#include "pageview.h"

#include <QFileInfo>
#include <QToolBar>

namespace mergen {
namespace {

// One string: compositor-drawn decorations have no separate zones to fill.
QString titleFor(const QString &fileName) {
    if (fileName.isEmpty()) {
        return QStringLiteral(R"(MERGEN ///— —\\\ MEGAS)");
    }
    return QStringLiteral(R"(MERGEN ///— %1 —\\\ MEGAS)").arg(fileName);
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    m_doc = std::make_unique<Document>();

    m_view = new PageView(this);
    setCentralWidget(m_view);

    buildToolBar();
    updateTitle();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildToolBar() {
    m_toolBar = addToolBar(QStringLiteral("MERGEN"));
    m_toolBar->setMovable(false);
    m_toolBar->setFloatable(false);
    m_toolBar->setContextMenuPolicy(Qt::PreventContextMenu);
}

void MainWindow::openPath(const QString &path) {
    const QFileInfo info(path);
    const LoadStatus status = m_doc->openPath(path);

    switch (status) {
    case LoadStatus::Ok:
        m_view->setDocument(m_doc.get());
        updateTitle();
        return;

    case LoadStatus::NotFound:
        closeDocument();
        m_view->setMessage(tr("%1 does not exist.").arg(info.fileName()));
        return;

    case LoadStatus::NoPermission:
        closeDocument();
        m_view->setMessage(tr("%1 cannot be read.").arg(info.fileName()));
        return;

    case LoadStatus::NeedsPassword:
        closeDocument();
        m_view->setMessage(tr("%1 is password-protected.").arg(info.fileName()));
        return;

    case LoadStatus::Invalid:
        closeDocument();
        m_view->setMessage(tr("%1 is not a PDF, or it is damaged.").arg(info.fileName()));
        return;
    }
}

void MainWindow::closeDocument() {
    m_doc->close();
    m_view->setDocument(nullptr);
    updateTitle();
}

void MainWindow::updateTitle() {
    const QString name = m_doc->isOpen() ? QFileInfo(m_doc->path()).fileName() : QString();
    setWindowTitle(titleFor(name));
}

} // namespace mergen
