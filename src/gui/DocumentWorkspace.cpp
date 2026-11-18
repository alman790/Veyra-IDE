#include "../application/DocumentController.hpp"
#include "MainWindow.hpp"
#include "ProjectFileModel.hpp"
#include "SourceInspector.hpp"
#include "SourceTemplates.hpp"
#include "UiIcons.hpp"
#include "UiLanguage.hpp"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextDocumentLayout>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <algorithm>

namespace {
CloseDecision decide(QWidget* parent, const EditorDocument& value) {
    const auto answer = QMessageBox::warning(parent, UiLanguage::tr("Unsaved document"),
                                             QString(UiLanguage::tr("Save changes to %1?")).arg(value.name),
                                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                             QMessageBox::Save);
    return answer == QMessageBox::Save      ? CloseDecision::Save
           : answer == QMessageBox::Discard ? CloseDecision::Discard
                                            : CloseDecision::Cancel;
}

QString savePath(QWidget* parent, const EditorDocument& value) {
    return QFileDialog::getSaveFileName(parent, UiLanguage::tr("Save ") + value.name,
                                        value.path.isEmpty() ? value.name : value.path, "Source files (*.*)");
}
}

void MainWindow::storeEditorState() {
    auto* value = documents_ ? documents_->activeDocument() : nullptr;
    if (!value || editor_->document() != value->text.get())
        return;
    const auto cursor = editor_->textCursor();
    value->position = cursor.position();
    value->anchor = cursor.anchor();
    value->verticalScroll = editor_->verticalScrollBar()->value();
    value->horizontalScroll = editor_->horizontalScrollBar()->value();
    value->trusted = trustedToRun_;
}

void MainWindow::updateDocumentTabs() {
    if (!documents_ || !documentTabs_)
        return;
    QSignalBlocker blocked(documentTabs_);
    while (documentTabs_->count() > documents_->count())
        documentTabs_->removeTab(documentTabs_->count() - 1);
    for (int i = 0; i < documents_->count(); ++i) {
        const auto* value = documents_->document(i);
        const auto title = value->name + (value->text->isModified() ? " ●" : "");
        if (i >= documentTabs_->count()) {
            documentTabs_->addTab(title);
            auto* old = documentTabs_->tabButton(i, QTabBar::RightSide);
            if (old)
                old->deleteLater();
            auto* close = new QToolButton(documentTabs_);
            close->setObjectName("documentClose");
            close->setProperty("tsIcon", "close");
            close->setIcon(tsui::icon("close", tsui::ThemeManager::color(tsui::Role::Muted)));
            close->setIconSize(QSize(12, 12));
            close->setStyleSheet(
                "QToolButton{min-width:16px;min-height:16px;max-width:16px;max-height:16px;padding:0;border:"
                "0;background:transparent;}QToolButton:hover{background:#41434A;border-radius:4px;}");
            close->setToolTip(UiLanguage::tr("Close document"));
            close->setAccessibleName(UiLanguage::tr("Close document"));
            documentTabs_->setTabButton(i, QTabBar::RightSide, close);
            connect(close, &QToolButton::clicked, this, [this, close] {
                for (int tab = 0; tab < documentTabs_->count(); ++tab)
                    if (documentTabs_->tabButton(tab, QTabBar::RightSide) == close) {
                        closeDocumentTab(tab);
                        return;
                    }
            });
        } else
            documentTabs_->setTabText(i, title);
        documentTabs_->setTabToolTip(i, value->path.isEmpty() ? UiLanguage::tr("Unsaved document")
                                                              : value->path);
    }
    documentTabs_->setCurrentIndex(documents_->activeIndex());
    updateWindowTitle();
}

void MainWindow::installDocumentWorkspace() {
    documents_ = new DocumentController(this);
    sessionSaveTimer_ = new QTimer(this);
    sessionSaveTimer_->setSingleShot(true);
    sessionSaveTimer_->setInterval(1500);
    sessionPath_ = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/editor-session.json";
    connect(sessionSaveTimer_, &QTimer::timeout, this, [this] {
        storeEditorState();
        QString error;
        if (!documents_->saveSession(sessionPath_, error))
            appendLog("Session: " + UiLanguage::text(error));
    });
    connect(documents_, &DocumentController::aboutToChangeActive, this, &MainWindow::storeEditorState);
    connect(documents_, &DocumentController::documentsChanged, this, [this] {
        updateDocumentTabs();
        sessionSaveTimer_->start();
    });
    connect(documents_, &DocumentController::documentChanged, this, [this](int index) {
        if (index < 0)
            return;
        updateDocumentTabs();
        sessionSaveTimer_->start();
        if (index == documents_->activeIndex()) {
            auto* value = documents_->document(index);
            sourcePath_ = value->path;
            sourceDisplayName_ = value->name;
            sourcePathLabel_->setText(
                (value->path.isEmpty() ? value->name : QDir(workspaceRoot_).relativeFilePath(value->path)) +
                (value->text->isModified() ? " ●" : ""));
            sourcePathLabel_->setToolTip(value->path);
        }
    });
    connect(documents_, &DocumentController::documentSaved, this, [this](const QString& path) {
        for (int index = 0; index < documents_->count(); ++index)
            if (documents_->document(index)->path == path && lsp_)
                lsp_->saveFile(path, documents_->document(index)->text->toPlainText());
    });
    connect(documents_, &DocumentController::documentClosed, this, [this](const QString& path) {
        if (lsp_ && !path.isEmpty())
            lsp_->closeFile(path);
        updateProjectTree();
    });
    connect(documents_, &DocumentController::activeChanged, this, [this](int index) {
        auto* value = documents_->document(index);
        if (!value) {
            showEmptyEditor();
            return;
        }
        emptyEditor_->hide();
        documentSplit_->show();
        sourcePathLabel_->parentWidget()->show();
        editor_->setReadOnly(false);
        if (sourceBuild_)
            sourceBuild_->setEnabled(true);
        if (sourceRun_)
            sourceRun_->setEnabled(true);
        switchingDocument_ = true;
        editor_->useDocument(value->text.get());
        sourcePath_ = value->path;
        sourceDisplayName_ = value->name;
        trustedToRun_ = value->trusted;
        sourcePathLabel_->setText(
            (value->path.isEmpty() ? value->name : QDir(workspaceRoot_).relativeFilePath(value->path)) +
            (value->text->isModified() ? " ●" : ""));
        sourcePathLabel_->setToolTip(value->path);
        auto cursor = editor_->textCursor();
        const int end = std::max(0, value->text->characterCount() - 1);
        cursor.setPosition(std::clamp(value->anchor, 0, end));
        cursor.setPosition(std::clamp(value->position, 0, end), QTextCursor::KeepAnchor);
        editor_->setTextCursor(cursor);
        const QPointer<QTextDocument> document = value->text.get();
        const int vertical = value->verticalScroll, horizontal = value->horizontalScroll;
        QTimer::singleShot(0, this, [this, document, vertical, horizontal] {
            if (document && editor_->document() == document) {
                editor_->verticalScrollBar()->setValue(vertical);
                editor_->horizontalScrollBar()->setValue(horizontal);
            }
        });
        syncLanguage(true);
        updateMarkdown(true);
        analyzeCode();
        updateDocumentTabs();
        revealActiveFile();
        switchingDocument_ = false;
    });
    connect(documentTabs_, &QTabBar::currentChanged, documents_, &DocumentController::activate);
    connect(documentTabs_, &QTabBar::tabMoved, documents_, &DocumentController::moveDocument);
    connect(documentTabs_, &QTabBar::tabCloseRequested, this, &MainWindow::closeDocumentTab);
    QString issue;
    documents_->restoreSession(sessionPath_, issue);
    if (!issue.isEmpty())
        appendLog("Session recovery: " + issue);
    if (documents_->count() == 0)
        showEmptyEditor();
    configureProjectFiles();
}

void MainWindow::openCode() {
    const auto paths =
        QFileDialog::getOpenFileNames(this, UiLanguage::tr("Open source files"), workspaceRoot_,
                                      "Source files (*.cpp *.hpp *.h *.cc *.cxx *.c);;All files (*)");
    for (const auto& path : paths)
        if (!loadCodeFile(path))
            break;
}

bool MainWindow::loadCodeFile(const QString& path) {
    QString error;
    if (!documents_->open(path, error)) {
        QMessageBox::warning(this, UiLanguage::tr("Open"), UiLanguage::text(error));
        return false;
    }
    goToPage(2);
    return true;
}

void MainWindow::newCode() {
    createProjectEntry(false);
}

void MainWindow::createProjectEntry(bool directory, const QString& base) {
    QString parent = base;
    if (parent.isEmpty() && projectTree_->currentIndex().isValid()) {
        const auto item = projectFilter_->mapToSource(projectTree_->currentIndex());
        const auto path = projectFiles_->filePath(item);
        parent = projectFiles_->isDir(item) ? path : QFileInfo(path).absolutePath();
    }
    if (parent.isEmpty())
        parent = workspaceRoot_;
    if (directory && parent.isEmpty()) {
        QMessageBox::information(this, UiLanguage::tr("New folder"),
                                 UiLanguage::tr("Open a project folder first."));
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName("newProjectEntry");
    dialog.setWindowTitle(directory ? UiLanguage::tr("New folder") : UiLanguage::tr("New file"));
    auto* form = new QFormLayout(&dialog);
    auto* name = new QLineEdit;
    name->setObjectName("newEntryName");
    name->setPlaceholderText(directory ? "folder" : "main.py");
    form->addRow(UiLanguage::tr("Name"), name);
    auto* language = new QComboBox(&dialog);
    language->setObjectName("newEntryLanguage");
    for (const auto& item : ts::availableLanguages())
        language->addItem(QString::fromStdString(item.name), int(item.id));
    const auto active =
        ts::languageForPath((sourcePath_.isEmpty() ? sourceDisplayName_ : sourcePath_).toStdString()).id;
    language->setCurrentIndex(std::max(0, language->findData(int(active))));
    if (!directory)
        form->addRow(UiLanguage::tr("File type"), language);
    else
        language->hide();
    auto* location = new QLabel(parent.isEmpty() ? UiLanguage::tr("Unsaved document") : parent);
    location->setWordWrap(true);
    form->addRow(UiLanguage::tr("Location"), location);
    auto* error = new QLabel;
    error->setWordWrap(true);
    form->addRow(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(name, &QLineEdit::textChanged, &dialog, [=](const QString& text) {
        const auto found = ts::languageForPath(text.toStdString()).id;
        if (found != ts::LanguageId::Unknown) {
            QSignalBlocker blocked(language);
            language->setCurrentIndex(language->findData(int(found)));
        }
    });
    connect(language, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [=](int) {
        const auto text = name->text();
        if (!QFileInfo(text).suffix().isEmpty()) {
            QSignalBlocker block(name);
            name->setText(QFileInfo(text).completeBaseName() + '.' +
                          sourceExtension(static_cast<ts::LanguageId>(language->currentData().toInt())));
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        auto value = name->text().trimmed();
        if (value.isEmpty() || value == "." || value == ".." || value.contains('/') || value.contains('\\')) {
            error->setText(UiLanguage::tr("Enter a valid name without path separators."));
            return;
        }
        const auto id = static_cast<ts::LanguageId>(language->currentData().toInt());
        if (!directory && QFileInfo(value).suffix().isEmpty())
            value += '.' + sourceExtension(id);
        if (parent.isEmpty()) {
            if (documents_->create(sourceTemplate(id, value), value) < 0) {
                error->setText(UiLanguage::tr("Open document limit reached."));
                return;
            }
            goToPage(2);
            dialog.accept();
            return;
        }
        const auto target = QDir(parent).filePath(value);
        if (QFileInfo::exists(target)) {
            error->setText(UiLanguage::tr("This path already exists."));
            return;
        }
        if (directory) {
            if (!QDir().mkdir(target)) {
                error->setText(UiLanguage::tr("Could not create folder."));
                return;
            }
        } else {
            QFile file(target);
            if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                error->setText(file.errorString());
                return;
            }
            const auto content = sourceTemplate(id, value).toUtf8();
            if (file.write(content) != content.size()) {
                error->setText(file.errorString());
                return;
            }
            file.close();
            loadCodeFile(target);
        }
        dialog.accept();
        updateProjectTree();
    });
    name->setFocus();
    dialog.resize(520, 240);
    dialog.exec();
}

void MainWindow::saveCode() {
    writeCode();
}

bool MainWindow::writeCode() {
    auto* value = documents_->activeDocument();
    if (!value)
        return false;
    const auto target = value->path.isEmpty() ? savePath(this, *value) : value->path;
    if (target.isEmpty())
        return false;
    QString error;
    if (!documents_->save(documents_->activeIndex(), target, error)) {
        QMessageBox::warning(this, UiLanguage::tr("Save"), UiLanguage::text(error));
        return false;
    }
    syncLanguage(true);
    updateProjectTree();
    statusBar()->showMessage(UiLanguage::tr("Source saved: ") + target, 5000);
    return true;
}

void MainWindow::saveCodeAs() {
    auto* value = documents_->activeDocument();
    if (!value)
        return;
    const auto path = savePath(this, *value);
    if (path.isEmpty())
        return;
    const auto old = value->path;
    QString error;
    if (!documents_->save(documents_->activeIndex(), path, error))
        QMessageBox::warning(this, UiLanguage::tr("Save"), UiLanguage::text(error));
    else {
        if (lsp_ && old != path)
            lsp_->closeFile(old);
        syncLanguage(true);
        updateProjectTree();
    }
}

void MainWindow::saveAllDocuments() {
    QString error;
    if (!documents_->saveAll([this](const auto& value) { return savePath(this, value); }, error) &&
        !error.isEmpty())
        QMessageBox::warning(this, UiLanguage::tr("Save all"), UiLanguage::text(error));
    syncLanguage(true);
    updateProjectTree();
}

void MainWindow::closeDocumentTab(int index) {
    documents_->activate(index);
    QString error;
    if (!documents_->close(
            index, [this](const auto& value) { return decide(this, value); },
            [this](const auto& value) { return savePath(this, value); }, error) &&
        !error.isEmpty())
        QMessageBox::warning(this, UiLanguage::tr("Closing"), UiLanguage::text(error));
}

bool MainWindow::confirmDiscardCode() {
    if (!documents_)
        return true;
    QString error;
    const bool allowed =
        documents_->prepareClose([this](const auto& value) { return decide(this, value); },
                                 [this](const auto& value) { return savePath(this, value); }, error);
    if (!allowed && !error.isEmpty())
        QMessageBox::warning(this, UiLanguage::tr("Documents"), UiLanguage::text(error));
    return allowed;
}

void MainWindow::configureProjectFiles() {
    projectTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(projectTree_, &QTreeView::customContextMenuRequested, this, [this](const QPoint& point) {
        const auto item = projectTree_->indexAt(point);
        const auto source = projectFilter_->mapToSource(item);
        const QString path = source.isValid() ? projectFiles_->filePath(source) : QString();
        const auto directory = QFileInfo(path).isDir() ? path
                               : !path.isEmpty()       ? QFileInfo(path).absolutePath()
                                                       : workspaceRoot_;
        QMenu menu;
        auto* create = menu.addAction(UiLanguage::tr("New file…"));
        auto* folder = menu.addAction(UiLanguage::tr("New folder…"));
        auto* rename = menu.addAction(UiLanguage::tr("Rename…"));
        auto* remove = menu.addAction(UiLanguage::tr("Move to Trash…"));
        rename->setEnabled(!path.isEmpty());
        remove->setEnabled(!path.isEmpty());
        folder->setEnabled(!directory.isEmpty());
        create->setEnabled(!directory.isEmpty());
        const auto selected = menu.exec(projectTree_->viewport()->mapToGlobal(point));
        if (!selected)
            return;
        if (selected == create || selected == folder) {
            createProjectEntry(selected == folder, directory);
        } else if (selected == rename) {
            bool accepted = false;
            const auto name =
                QInputDialog::getText(this, UiLanguage::tr("Rename"), UiLanguage::tr("New name:"),
                                      QLineEdit::Normal, QFileInfo(path).fileName(), &accepted)
                    .trimmed();
            if (!accepted || name.isEmpty())
                return;
            if (name.contains('/') || name.contains('\\') || name == "." || name == "..")
                return;
            QStringList oldPaths;
            for (int i = 0; i < documents_->count(); ++i) {
                const auto& old = documents_->document(i)->path;
                if (old == path || old.startsWith(path + '/'))
                    oldPaths << old;
            }
            QString error;
            if (!documents_->renamePath(path, QFileInfo(path).dir().filePath(name), error))
                QMessageBox::warning(this, UiLanguage::tr("Rename"), UiLanguage::text(error));
            else {
                for (const auto& old : oldPaths)
                    if (lsp_)
                        lsp_->closeFile(old);
                auto* value = documents_->activeDocument();
                if (value) {
                    sourcePath_ = value->path;
                    sourceDisplayName_ = value->name;
                    sourcePathLabel_->setText(value->path.isEmpty()
                                                  ? value->name
                                                  : QDir(workspaceRoot_).relativeFilePath(value->path));
                    syncLanguage(true);
                    analyzeCode();
                }
            }
        } else if (selected == remove) {
            if (QMessageBox::warning(
                    this, UiLanguage::tr("Delete"),
                    QString(UiLanguage::tr("Move %1 to Trash? Open documents will be closed.")).arg(path),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
                return;
            for (int i = documents_->count() - 1; i >= 0; --i) {
                const auto* value = documents_->document(i);
                if (value->path == path || value->path.startsWith(path + '/')) {
                    const int before = documents_->count();
                    const QString affectedPath = value->path;
                    closeDocumentTab(i);
                    if (documents_->count() == before && documents_->document(i) &&
                        documents_->document(i)->path == affectedPath)
                        return;
                }
            }
            if (!QFile::moveToTrash(path))
                QMessageBox::warning(
                    this, UiLanguage::tr("Trash"),
                    UiLanguage::tr("Could not move this path to Trash; it was not deleted."));
        }
        updateProjectTree();
    });
}

void MainWindow::showEmptyEditor() {
    switchingDocument_ = true;
    if (!emptyDocument_) {
        emptyDocument_ = new QTextDocument(this);
        emptyDocument_->setDocumentLayout(new QPlainTextDocumentLayout(emptyDocument_));
    }
    editor_->useDocument(emptyDocument_);
    editor_->setReadOnly(true);
    sourcePath_.clear();
    sourceDisplayName_.clear();
    lspSourcePath_.clear();
    if (lsp_)
        lsp_->stop();
    if (codeAuditTimer_)
        codeAuditTimer_->stop();
    if (sourceInspector_)
        sourceInspector_->cancel();
    currentCodeReport_ = {};
    threadHint_->hide();
    emptyEditor_->show();
    documentSplit_->hide();
    sourcePathLabel_->parentWidget()->hide();
    codeSearchBar_->hide();
    activeLanguage_->clear();
    editorPosition_->clear();
    if (sourceBuild_)
        sourceBuild_->setEnabled(false);
    if (sourceRun_)
        sourceRun_->setEnabled(false);
    updateWindowTitle();
    switchingDocument_ = false;
}
