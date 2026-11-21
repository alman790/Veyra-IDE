#include "RemoteWorkspace.hpp"
#include "../application/SourceRevision.hpp"
#include "CodeEditor.hpp"
#include "ThemeManager.hpp"
#include "ToolDiscovery.hpp"
#include "UiLanguage.hpp"
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

RemoteWorkspace::RemoteWorkspace(QWidget* parent) : QMainWindow(parent) {
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName("remoteWorkspace");
    setWindowTitle(UiLanguage::tr("Remote development"));
    resize(1280, 850);
    setStyleSheet(qApp->styleSheet());
    files_ = new RemoteSession(this);
    commands_ = new RemoteSession(this);
    lsp_ = new LspClient(this);
    auto* central = new QWidget;
    auto* layout = new QVBoxLayout(central);
    setCentralWidget(central);
    auto* bar = new QHBoxLayout;
    connect_ = new QPushButton(UiLanguage::tr("Connect to server…"));
    connect_->setObjectName("remoteConnect");
    save_ = new QPushButton(UiLanguage::tr("Save to server"));
    save_->setObjectName("remoteSave");
    auto* refresh = new QPushButton(UiLanguage::tr("Reload file"));
    auto* compare = new QPushButton(UiLanguage::tr("Compare with server…"));
    auto* cancelTransfer = new QPushButton(UiLanguage::tr("Cancel transfer"));
    cancelTransfer->setEnabled(false);
    auto* create = new QPushButton(UiLanguage::tr("New remote file…"));
    discover_ = new QPushButton(UiLanguage::tr("Discover server tools…"));
    bar->addWidget(connect_);
    bar->addWidget(save_);
    bar->addWidget(refresh);
    bar->addWidget(compare);
    bar->addWidget(cancelTransfer);
    bar->addWidget(create);
    bar->addStretch();
    bar->addWidget(discover_);
    layout->addLayout(bar);
    auto* horizontal = new QSplitter;
    tree_ = new QTreeWidget;
    tree_->setHeaderLabel(UiLanguage::tr("Remote project"));
    tree_->setObjectName("remoteFiles");
    horizontal->addWidget(tree_);
    auto* right = new QSplitter(Qt::Vertical);
    tabs_ = new QTabWidget;
    tabs_->setTabsClosable(true);
    tabs_->setObjectName("remoteDocuments");
    right->addWidget(tabs_);
    auto* terminal = new QWidget;
    auto* terminalLayout = new QVBoxLayout(terminal);
    auto* commandBar = new QHBoxLayout;
    command_ = new QLineEdit;
    command_->setObjectName("remoteCommand");
    command_->setPlaceholderText(UiLanguage::tr("Build or shell command, executed in the remote project"));
    run_ = new QPushButton(UiLanguage::tr("Run on server"));
    stop_ = new QPushButton(UiLanguage::tr("Disconnect command"));
    commandBar->addWidget(command_, 1);
    commandBar->addWidget(run_);
    commandBar->addWidget(stop_);
    terminalLayout->addLayout(commandBar);
    output_ = new QPlainTextEdit;
    output_->setReadOnly(true);
    output_->setObjectName("remoteOutput");
    output_->document()->setMaximumBlockCount(3000);
    terminalLayout->addWidget(output_);
    auto* inputBar = new QHBoxLayout;
    stdin_ = new QLineEdit;
    stdin_->setPlaceholderText(UiLanguage::tr("Remote command input · Enter"));
    auto* eof = new QPushButton("EOF");
    inputBar->addWidget(stdin_, 1);
    inputBar->addWidget(eof);
    terminalLayout->addLayout(inputBar);
    right->addWidget(terminal);
    right->setSizes({580, 180});
    horizontal->addWidget(right);
    horizontal->setSizes({250, 1000});
    layout->addWidget(horizontal, 1);
    auto* language = new QHBoxLayout;
    serverCommand_ = new QLineEdit;
    serverCommand_->setObjectName("remoteLanguageServer");
    serverCommand_->setPlaceholderText(UiLanguage::tr("Language server command on this server"));
    sdkRoot_ = new QLineEdit;
    sdkRoot_->setPlaceholderText(UiLanguage::tr("Remote SDK root (optional)"));
    languageConnect_ = new QPushButton(UiLanguage::tr("Connect language server"));
    language->addWidget(serverCommand_, 2);
    language->addWidget(sdkRoot_, 1);
    language->addWidget(languageConnect_);
    layout->addLayout(language);
    status_ =
        new QLabel(UiLanguage::tr("Connect using your SSH configuration, identity key or local SSH agent."));
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);
    auto* file = menuBar()->addMenu(UiLanguage::tr("&File"));
    auto* saveAction = file->addAction(UiLanguage::tr("Save to server"), this, &RemoteWorkspace::saveCurrent);
    saveAction->setShortcut(QKeySequence::Save);
    auto* all = file->addAction(UiLanguage::tr("Save all"), this, &RemoteWorkspace::saveAll);
    all->setShortcut(QKeySequence("Ctrl+Shift+S"));
    file->addAction(UiLanguage::tr("New remote file…"), this, &RemoteWorkspace::newFile);
    file->addAction(UiLanguage::tr("Reload file"), this, &RemoteWorkspace::reload);
    file->addAction(UiLanguage::tr("Close window"), this, &QWidget::close);
    auto* code = menuBar()->addMenu(UiLanguage::tr("&Code"));
    auto* definition =
        code->addAction(UiLanguage::tr("Go to definition"), this, &RemoteWorkspace::navigateDefinition);
    definition->setShortcut(QKeySequence("F12"));
    auto* format = code->addAction(UiLanguage::tr("Reformat code"), this, &RemoteWorkspace::formatCurrent);
    format->setShortcut(QKeySequence("Ctrl+Alt+L"));
    connect(connect_, &QPushButton::clicked, this, &RemoteWorkspace::configure);
    connect(save_, &QPushButton::clicked, this, &RemoteWorkspace::saveCurrent);
    connect(refresh, &QPushButton::clicked, this, &RemoteWorkspace::reload);
    connect(compare, &QPushButton::clicked, this, &RemoteWorkspace::compareCurrent);
    connect(files_, &RemoteSession::busyChanged, cancelTransfer, &QPushButton::setEnabled);
    connect(cancelTransfer, &QPushButton::clicked, this, [this] {
        files_->cancel();
        saveQueue_.clear();
        comparisonPath_.clear();
        closeAfterSave_ = false;
        for (auto it = documents_.begin(); it != documents_.end(); ++it) {
            it->uploadingRevision = -1;
            it->uploadedText.clear();
            it->loading = false;
            if (it->editor)
                it->editor->setReadOnly(false);
        }
        status_->setText(UiLanguage::tr("Transfer cancelled. Unsaved editor changes were kept."));
        updateTabs();
    });
    connect(create, &QPushButton::clicked, this, &RemoteWorkspace::newFile);
    connect(discover_, &QPushButton::clicked, this, &RemoteWorkspace::discoverTools);
    connect(languageConnect_, &QPushButton::clicked, this, [this] {
        if (currentLanguage_.isEmpty())
            return;
        languageCommands_[currentLanguage_] = serverCommand_->text().trimmed();
        sdkRoots_[currentLanguage_] = sdkRoot_->text().trimmed();
        startLanguageServer();
    });
    connect(tabs_, &QTabWidget::currentChanged, this, [this] { activate(); });
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
        auto* editor = qobject_cast<CodeEditor*>(tabs_->widget(index));
        if (!editor)
            return;
        const auto path = editor->property("remotePath").toString();
        if (documents_[path].uploadingRevision >= 0 || documents_[path].loading) {
            status_->setText(UiLanguage::tr("Wait for the current remote file request."));
            return;
        }
        if (editor->document()->isModified() &&
            QMessageBox::question(this, UiLanguage::tr("Unsaved remote file"),
                                  UiLanguage::tr("Discard this file's unsaved changes?"),
                                  QMessageBox::Discard | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Discard)
            return;
        lsp_->closeFile(files_->connection().root + '/' + path);
        documents_.remove(path);
        tabs_->removeTab(index);
        editor->deleteLater();
    });
    connect(tree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        if (item->data(0, Qt::UserRole + 1).toString() == "d" && !item->data(0, Qt::UserRole + 2).toBool() &&
            !files_->busy())
            files_->list(item->data(0, Qt::UserRole).toString());
    });
    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        if (item->data(0, Qt::UserRole + 1).toString() == "f")
            open(item->data(0, Qt::UserRole).toString());
    });
    connect(files_, &RemoteSession::connected, this, [this](const QString& root) {
        connected_ = true;
        commands_->configure(files_->connection());
        setWindowTitle(files_->connection().host + " · " + root + " — Veyra IDE");
        status_->setText(UiLanguage::tr("Connected to remote project."));
        files_->list();
        updateControls();
    });
    connect(files_, &RemoteSession::directoryReady, this, &RemoteWorkspace::populate);
    displayFile_ = [this](const QString& path, const QByteArray& bytes, const QString& fingerprint) {
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString text = decoder(bytes);
        if (decoder.hasError() || text.contains(QChar(0)) || bytes.size() > 32 * 1024 * 1024) {
            if (documents_.contains(path)) {
                documents_[path].loading = false;
                if (documents_[path].editor)
                    documents_[path].editor->setReadOnly(false);
            }
            status_->setText(UiLanguage::tr("The remote file must be UTF-8 text smaller than 32 MiB."));
            return;
        }
        CodeEditor* editor = nullptr;
        if (documents_.contains(path))
            editor = documents_[path].editor;
        if (!editor) {
            editor = new CodeEditor;
            editor->setGitAnnotationsAvailable(false);
            editor->setProperty("remotePath", path);
            editor->setObjectName("remoteCodeEditor");
            editor->setLanguage(ts::languageForPath(path.toStdString()).id);
            documents_.insert(path, {editor, fingerprint, -1, false, {}});
            tabs_->addTab(editor, path.section('/', -1));
            auto* changeTimer = new QTimer(editor);
            changeTimer->setSingleShot(true);
            changeTimer->setInterval(250);
            connect(editor, &CodeEditor::sourceTextChanged, this, [this, editor, changeTimer] {
                updateTabs();
                if (currentEditor() == editor) {
                    editor->setDiagnostics({});
                    changeTimer->start();
                }
            });
            connect(changeTimer, &QTimer::timeout, this, [this, editor] {
                if (currentEditor() == editor && lsp_->ready())
                    lsp_->changeText(editor->toPlainText());
            });
            editor->setCompletionRequest([this, editor] {
                if (currentEditor() != editor || !lsp_->ready())
                    return;
                lsp_->changeText(editor->toPlainText());
                completionEditor_ = editor;
                completionPosition_ = editor->textCursor().position();
                completionRevision_ = sourceRevision(editor->document());
                lsp_->requestCompletion(editor->textCursor().blockNumber(),
                                        editor->textCursor().positionInBlock());
            });
        }
        auto& document = documents_[path];
        document.fingerprint = fingerprint;
        document.loading = false;
        document.crlf = bytes.contains("\r\n") && bytes.count('\n') == bytes.count("\r\n");
        document.utf8Bom = bytes.startsWith("\xef\xbb\xbf");
        editor->setReadOnly(false);
        editor->setPlainText(text);
        editor->document()->setModified(false);
        tabs_->setCurrentWidget(editor);
        activate();
        updateTabs();
        editor->setFocus();
    };
    connect(files_, &RemoteSession::fileReady, this,
            [this](const QString& path, const QByteArray& bytes, const QString& fingerprint) {
                if (comparisonPath_ == path) {
                    comparisonPath_.clear();
                    showComparison(path, bytes, fingerprint);
                } else
                    displayFile_(path, bytes, fingerprint);
            });
    connect(files_, &RemoteSession::saved, this, [this](const QString& path, const QString& fingerprint) {
        auto found = documents_.find(path);
        if (found != documents_.end() && found->editor) {
            found->fingerprint = fingerprint;
            if (sourceRevision(found->editor->document()) == found->uploadingRevision)
                found->editor->document()->setModified(false);
            found->uploadingRevision = -1;
            lsp_->saveFile(files_->connection().root + '/' + path, found->uploadedText);
            if (currentEditor() == found->editor)
                lsp_->changeText(found->editor->toPlainText());
            found->uploadedText.clear();
        }
        if (!saveQueue_.isEmpty() && saveQueue_.first() == path)
            saveQueue_.removeFirst();
        status_->setText(UiLanguage::tr("Saved to server."));
        updateTabs();
        pumpSave();
    });
    connect(files_, &RemoteSession::failed, this, [this](const QString& message) {
        comparisonPath_.clear();
        for (auto it = documents_.begin(); it != documents_.end(); ++it) {
            it->uploadingRevision = -1;
            it->uploadedText.clear();
            if (it->loading && it->editor) {
                it->loading = false;
                it->editor->setReadOnly(false);
            }
        }
        saveQueue_.clear();
        closeAfterSave_ = false;
        status_->setText(UiLanguage::text(message));
        const auto path = files_->activePath();
        if (message.startsWith("The remote file changed") && documents_.contains(path)) {
            tabs_->setCurrentWidget(documents_[path].editor);
            QTimer::singleShot(0, this, &RemoteWorkspace::compareCurrent);
        }
        updateTabs();
    });
    connect(files_, &RemoteSession::busyChanged, this, [this] { updateControls(); });
    connect(commands_, &RemoteSession::busyChanged, this, [this] { updateControls(); });
    connect(commands_, &RemoteSession::commandOutput, this, &RemoteWorkspace::appendOutput);
    connect(commands_, &RemoteSession::commandFinished, this, [this](int code) {
        appendOutput(("\n" + UiLanguage::tr("Remote command exited: %1").arg(code) + "\n").toUtf8());
    });
    connect(commands_, &RemoteSession::failed, this,
            [this](const QString& error) { status_->setText(UiLanguage::text(error)); });
    connect(run_, &QPushButton::clicked, this, [this] {
        if (command_->text().trimmed().isEmpty())
            return;
        outputDecoder_.resetState();
        appendOutput(("> " + command_->text() + "\n").toUtf8());
        commands_->run(command_->text());
    });
    connect(command_, &QLineEdit::returnPressed, run_, &QPushButton::click);
    connect(stop_, &QPushButton::clicked, this, [this] {
        commands_->cancel();
        status_->setText(UiLanguage::tr("SSH command disconnected. Server processes may continue running."));
    });
    connect(stdin_, &QLineEdit::returnPressed, this, [this] {
        commands_->sendInput(stdin_->text());
        stdin_->clear();
    });
    connect(eof, &QPushButton::clicked, commands_, &RemoteSession::finishInput);
    updateControls();
}

RemoteWorkspace::~RemoteWorkspace() {
    lsp_->stop();
    files_->cancel();
    commands_->cancel();
}

CodeEditor* RemoteWorkspace::currentEditor() const {
    return qobject_cast<CodeEditor*>(tabs_->currentWidget());
}

QString RemoteWorkspace::currentPath() const {
    const auto* editor = currentEditor();
    return editor ? editor->property("remotePath").toString() : QString();
}

bool RemoteWorkspace::modified() const {
    for (const auto& doc : documents_)
        if (doc.editor && doc.editor->document()->isModified())
            return true;
    return false;
}

void RemoteWorkspace::updateControls() {
    const bool idle = !files_->busy();
    connect_->setEnabled(idle && !connected_);
    save_->setEnabled(connected_ && idle && currentEditor());
    tree_->setEnabled(connected_ && idle);
    discover_->setEnabled(connected_ && idle);
    languageConnect_->setEnabled(connected_ && currentEditor());
    run_->setEnabled(connected_ && !commands_->busy());
    stop_->setEnabled(commands_->busy());
    stdin_->setEnabled(commands_->busy());
}

void RemoteWorkspace::updateTabs() {
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* editor = qobject_cast<CodeEditor*>(tabs_->widget(i));
        if (editor)
            tabs_->setTabText(i, editor->property("remotePath").toString().section('/', -1) +
                                     (editor->document()->isModified() ? " ●" : ""));
    }
    updateControls();
}

void RemoteWorkspace::populate(const QString& path, const QJsonArray& entries) {
    QTreeWidgetItem* parent = nullptr;
    if (path.isEmpty()) {
        tree_->clear();
        parent = new QTreeWidgetItem(tree_, {files_->connection().root});
        parent->setData(0, Qt::UserRole, QString());
        parent->setData(0, Qt::UserRole + 1, "d");
    } else {
        QTreeWidgetItemIterator it(tree_);
        while (*it) {
            if ((*it)->data(0, Qt::UserRole).toString() == path) {
                parent = *it;
                break;
            }
            ++it;
        }
    }
    if (!parent)
        return;
    qDeleteAll(parent->takeChildren());
    parent->setData(0, Qt::UserRole + 2, true);
    QList<QJsonObject> sorted;
    for (const auto& entry : entries)
        sorted.append(entry.toObject());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        const bool ad = a.value("kind") == "d", bd = b.value("kind") == "d";
        if (ad != bd)
            return ad;
        return a.value("name").toString().localeAwareCompare(b.value("name").toString()) < 0;
    });
    for (const auto& entry : sorted) {
        const auto name = entry.value("name").toString();
        const auto kind = entry.value("kind").toString();
        auto* item = new QTreeWidgetItem(parent, {name + (kind == "l" ? " ↗" : "")});
        item->setData(0, Qt::UserRole, path.isEmpty() ? name : path + '/' + name);
        item->setData(0, Qt::UserRole + 1, kind);
        if (kind == "d")
            item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        if (kind == "l")
            item->setToolTip(0, UiLanguage::tr("Symbolic links are not traversed."));
    }
    parent->setExpanded(true);
}

void RemoteWorkspace::open(const QString& path) {
    if (documents_.contains(path) && documents_[path].editor) {
        tabs_->setCurrentWidget(documents_[path].editor);
        return;
    }
    if (!files_->busy())
        files_->read(path);
}

void RemoteWorkspace::activate() {
    auto* editor = currentEditor();
    currentLanguage_ =
        editor ? QString::fromStdString(ts::languageForPath(currentPath().toStdString()).lspId) : QString();
    serverCommand_->setText(languageCommands_.value(currentLanguage_));
    sdkRoot_->setText(sdkRoots_.value(currentLanguage_));
    if (editor && !serverCommand_->text().isEmpty())
        startLanguageServer();
    updateControls();
}

void RemoteWorkspace::startLanguageServer() {
    auto* editor = currentEditor();
    if (!editor || !connected_ || serverCommand_->text().trimmed().isEmpty())
        return;
    const auto config = files_->connection();
    auto args = RemoteSession::arguments(config);
    args << "cd -P " + RemoteSession::quote(config.root) + " && exec " + serverCommand_->text();
    lsp_->setTransport({RemoteSession::sshProgram(config), config.root, sdkRoot_->text(), args});
    auto highlights = std::make_shared<ts::Array<ts::Diagnostic>>();
    lsp_->startFile(
        config.root + '/' + currentPath(), editor->toPlainText(),
        ts::languageForPath(currentPath().toStdString()),
        [this](const QString& text) { status_->setText(UiLanguage::text(text)); },
        [this, editor](const QStringList& values) {
            if (currentEditor() == editor && completionEditor_ == editor &&
                sourceRevision(editor->document()) == completionRevision_ &&
                editor->textCursor().position() == completionPosition_)
                editor->setCompletionWords(values, lsp_->completionEdits());
        },
        [this, editor, highlights](const QString& message, int line, const QString&) {
            if (currentEditor() != editor)
                return;
            if (line == 0)
                highlights->clear();
            else
                highlights->pushBack(
                    {message.startsWith("[error]") ? ts::RiskLevel::Critical : ts::RiskLevel::Warning, line,
                     "LSP", "LSP", message.toStdString(), ""});
            editor->setDiagnostics(*highlights);
        });
}

void RemoteWorkspace::saveCurrent() {
    const auto path = currentPath();
    if (path.isEmpty() || files_->busy())
        return;
    saveQueue_ = {path};
    pumpSave();
}

void RemoteWorkspace::saveAll() {
    if (files_->busy())
        return;
    saveQueue_.clear();
    for (auto it = documents_.begin(); it != documents_.end(); ++it)
        if (it->editor && it->editor->document()->isModified())
            saveQueue_.append(it.key());
    pumpSave();
}

void RemoteWorkspace::pumpSave() {
    if (files_->busy())
        return;
    if (saveQueue_.isEmpty()) {
        if (closeAfterSave_) {
            closeAfterSave_ = false;
            if (!modified())
                close();
            else
                status_->setText(UiLanguage::tr("Files changed during upload. Save them before closing."));
        }
        return;
    }
    const auto path = saveQueue_.first();
    auto found = documents_.find(path);
    if (found == documents_.end() || !found->editor) {
        saveQueue_.removeFirst();
        pumpSave();
        return;
    }
    found->uploadingRevision = sourceRevision(found->editor->document());
    found->uploadedText = found->editor->toPlainText();
    status_->setText(UiLanguage::tr("Uploading remote file…"));
    auto bytes = found->uploadedText.toUtf8();
    if (found->crlf)
        bytes.replace("\n", "\r\n");
    if (found->utf8Bom)
        bytes.prepend("\xef\xbb\xbf");
    files_->write(path, bytes, found->fingerprint);
}

void RemoteWorkspace::reload() {
    const auto path = currentPath();
    if (path.isEmpty() || files_->busy())
        return;
    auto& doc = documents_[path];
    if (doc.editor->document()->isModified() &&
        QMessageBox::question(this, UiLanguage::tr("Reload remote file"),
                              UiLanguage::tr("Discard local edits and reload this file from the server?"),
                              QMessageBox::Discard | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Discard)
        return;
    doc.loading = true;
    doc.editor->setReadOnly(true);
    files_->read(path);
}

void RemoteWorkspace::newFile() {
    if (!connected_ || files_->busy())
        return;
    bool ok = false;
    const auto path = QInputDialog::getText(this, UiLanguage::tr("New remote file"),
                                            UiLanguage::tr("Path relative to the remote project"),
                                            QLineEdit::Normal, {}, &ok);
    if (!ok || !RemoteSession::validPath(path) || documents_.contains(path))
        return;
    displayFile_(path, {}, "missing");
    if (auto* editor = currentEditor())
        editor->document()->setModified(true);
    updateTabs();
}

void RemoteWorkspace::appendOutput(const QByteArray& bytes) {
    const auto text = outputDecoder_(bytes);
    auto cursor = output_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    output_->setTextCursor(cursor);
    output_->ensureCursorVisible();
}

void RemoteWorkspace::closeEvent(QCloseEvent* event) {
    if (files_->busy()) {
        status_->setText(UiLanguage::tr("Wait for the current remote file request."));
        event->ignore();
        return;
    }
    if (modified()) {
        const auto answer = QMessageBox::question(
            this, UiLanguage::tr("Unsaved remote files"),
            UiLanguage::tr("Save edited files to the server before closing?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (answer == QMessageBox::Cancel) {
            event->ignore();
            return;
        }
        if (answer == QMessageBox::Save) {
            closeAfterSave_ = true;
            saveAll();
            event->ignore();
            return;
        }
    }
    files_->cancel();
    commands_->cancel();
    lsp_->stop();
    event->accept();
}

void RemoteWorkspace::navigateDefinition() {
    const QPointer<CodeEditor> editor = currentEditor();
    if (!editor || !lsp_->ready())
        return;
    const auto revision = sourceRevision(editor->document());
    const auto position = editor->textCursor().position();
    lsp_->requestDocument(
        "textDocument/definition", editor->textCursor().blockNumber(), editor->textCursor().positionInBlock(),
        {}, [this, editor, revision, position](const QJsonValue& value) {
            if (!editor || currentEditor() != editor || sourceRevision(editor->document()) != revision ||
                editor->textCursor().position() != position)
                return;
            const auto location =
                (value.isArray() ? (value.toArray().isEmpty() ? QJsonValue() : value.toArray().at(0)) : value)
                    .toObject();
            const auto path =
                QUrl(location.value("targetUri").toString(location.value("uri").toString())).path();
            const auto root = files_->connection().root;
            if (!path.startsWith(root + '/')) {
                status_->setText(UiLanguage::tr("The definition is outside this remote project."));
                return;
            }
            const auto range =
                location.value("targetSelectionRange").toObject(location.value("range").toObject());
            const auto target = path.mid(root.size() + 1);
            const int line = range.value("start").toObject().value("line").toInt() + 1;
            if (documents_.contains(target)) {
                open(target);
                currentEditor()->jumpToLine(line);
            } else {
                auto connection = std::make_shared<QMetaObject::Connection>();
                *connection = connect(files_, &RemoteSession::fileReady, this,
                                      [this, target, line, connection](const QString& found,
                                                                       const QByteArray&, const QString&) {
                                          if (found == target) {
                                              disconnect(*connection);
                                              if (currentEditor())
                                                  currentEditor()->jumpToLine(line);
                                          }
                                      });
                open(target);
            }
        });
}

void RemoteWorkspace::formatCurrent() {
    const QPointer<CodeEditor> editor = currentEditor();
    if (!editor || !lsp_->ready())
        return;
    const auto revision = sourceRevision(editor->document());
    lsp_->changeText(editor->toPlainText());
    lsp_->requestDocument(
        "textDocument/formatting", -1, -1, {{"options", QJsonObject{{"tabSize", 4}, {"insertSpaces", true}}}},
        [this, editor, revision](const QJsonValue& value) {
            if (!editor || currentEditor() != editor || sourceRevision(editor->document()) != revision)
                return;
            struct Edit {
                int begin, end;
                QString text;
            };
            QList<Edit> edits;
            for (const auto& raw : value.toArray()) {
                const auto item = raw.toObject(), range = item.value("range").toObject();
                const auto start = range.value("start").toObject(), end = range.value("end").toObject();
                const auto first = editor->document()->findBlockByNumber(start.value("line").toInt(-1)),
                           last = editor->document()->findBlockByNumber(end.value("line").toInt(-1));
                const int a = start.value("character").toInt(-1), b = end.value("character").toInt(-1);
                if (!first.isValid() || !last.isValid() || a < 0 || b < 0 || a > first.text().size() ||
                    b > last.text().size())
                    return;
                edits.append({first.position() + a, last.position() + b, item.value("newText").toString()});
            }
            std::sort(edits.begin(), edits.end(),
                      [](const auto& a, const auto& b) { return a.begin > b.begin; });
            int boundary = editor->document()->characterCount();
            for (const auto& edit : edits) {
                if (edit.end < edit.begin || edit.end > boundary)
                    return;
                boundary = edit.begin;
            }
            auto cursor = editor->textCursor();
            cursor.beginEditBlock();
            for (const auto& edit : edits) {
                cursor.setPosition(edit.begin);
                cursor.setPosition(edit.end, QTextCursor::KeepAnchor);
                cursor.insertText(edit.text);
            }
            cursor.endEditBlock();
        });
}

void RemoteWorkspace::configure() {
    if (files_->busy())
        return;
    QDialog dialog(this);
    dialog.setObjectName("remoteConnectionDialog");
    dialog.setWindowTitle(UiLanguage::tr("SSH project connection"));
    auto* form = new QFormLayout(&dialog);
    QSettings settings;
    auto* host = new QLineEdit(settings.value("remote/lastHost").toString());
    host->setObjectName("remoteHost");
    host->setPlaceholderText(UiLanguage::tr("Host alias from SSH config, or hostname"));
    auto* user = new QLineEdit(settings.value("remote/lastUser").toString());
    user->setPlaceholderText(UiLanguage::tr("From SSH configuration"));
    auto* root = new QLineEdit(settings.value("remote/lastRoot", "~").toString());
    root->setObjectName("remoteRoot");
    auto* port = new QSpinBox;
    port->setRange(0, 65535);
    port->setValue(settings.value("remote/lastPort", 0).toInt());
    port->setSpecialValueText(UiLanguage::tr("SSH configuration"));
    auto* identity = new QLineEdit(settings.value("remote/lastIdentity").toString());
    identity->setPlaceholderText(UiLanguage::tr("Optional; otherwise use SSH config and agent"));
    auto* browse = new QPushButton(UiLanguage::tr("Browse…"));
    auto* row = new QHBoxLayout;
    row->addWidget(identity, 1);
    row->addWidget(browse);
    auto* executable = new QLineEdit(ToolDiscovery::find("ssh"));
    executable->setObjectName("remoteSshExecutable");
    form->addRow(UiLanguage::tr("Host"), host);
    form->addRow(UiLanguage::tr("User"), user);
    form->addRow(UiLanguage::tr("Remote project path"), root);
    form->addRow(UiLanguage::tr("Port"), port);
    form->addRow(UiLanguage::tr("Identity key"), row);
    form->addRow(UiLanguage::tr("OpenSSH executable"), executable);
    auto* trust = new QPushButton(UiLanguage::tr("Verify host key…"));
    form->addRow(trust);
    auto* note = new QLabel(
        UiLanguage::tr("The server must provide a POSIX shell. Authentication uses an identity key or your "
                       "local SSH agent; unlock encrypted keys in the agent. Existing SSH configuration and "
                       "jump hosts are used. Host key verification stays enabled."));
    note->setWordWrap(true);
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    auto connection = [=] {
        return RemoteConnection{host->text().trimmed(),
                                user->text().trimmed(),
                                root->text(),
                                identity->text(),
                                {},
                                executable->text(),
                                port->value(),
                                {}};
    };
    connect(browse, &QPushButton::clicked, &dialog, [=, this] {
        const auto file = QFileDialog::getOpenFileName(this, UiLanguage::tr("Identity key"));
        if (!file.isEmpty())
            identity->setText(file);
    });
    connect(trust, &QPushButton::clicked, &dialog, [=, this] { trustHost(connection()); });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!host->text().trimmed().isEmpty() && !root->text().trimmed().isEmpty())
            dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(620, 400);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto config = connection();
    settings.setValue("remote/lastHost", config.host);
    settings.setValue("remote/lastUser", config.user);
    settings.setValue("remote/lastRoot", config.root);
    settings.setValue("remote/lastPort", config.port);
    settings.setValue("remote/lastIdentity", config.identity);
    files_->configure(config);
    files_->connectTo();
}

void RemoteWorkspace::trustHost(const RemoteConnection& config) {
    if (config.host.isEmpty() || config.host.startsWith('-'))
        return;
    const auto keyscan = ToolDiscovery::find("ssh-keyscan"), keygen = ToolDiscovery::find("ssh-keygen");
    if (keyscan.isEmpty() || keygen.isEmpty()) {
        status_->setText(
            UiLanguage::tr("OpenSSH keyscan and keygen tools are required to verify a new host."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(UiLanguage::tr("Verify SSH host key"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* note =
        new QLabel(UiLanguage::tr("Verify these fingerprints with the server administrator before trusting "
                                  "the host. Existing SSH known_hosts files remain available."));
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* fingerprints = new QPlainTextEdit;
    fingerprints->setReadOnly(true);
    layout->addWidget(fingerprints);
    auto* state = new QLabel(UiLanguage::tr("Reading SSH configuration…"));
    state->setWordWrap(true);
    state->setTextFormat(Qt::PlainText);
    layout->addWidget(state);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto* accept = buttons->addButton(UiLanguage::tr("Trust host key"), QDialogButtonBox::AcceptRole);
    accept->setEnabled(false);
    layout->addWidget(buttons);
    QTemporaryDir temporary;
    QProcess process;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(15000);
    int stage = 0;
    QByteArray keys;
    QString alias;
    int actualPort = 22;
    connect(&timeout, &QTimer::timeout, &dialog, [&] {
        process.kill();
        state->setText(UiLanguage::tr("Host verification timed out."));
    });
    connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &dialog,
            [&](int code, QProcess::ExitStatus) {
                timeout.stop();
                const auto text = process.readAllStandardOutput();
                const auto errors = process.readAllStandardError();
                if (code != 0) {
                    state->setText(QString::fromUtf8(errors).left(4000));
                    return;
                }
                if (stage == 0) {
                    QHash<QString, QString> options;
                    for (const auto& line : QString::fromUtf8(text).split('\n')) {
                        const auto space = line.indexOf(' ');
                        if (space > 0)
                            options[line.left(space)] = line.mid(space + 1).trimmed();
                    }
                    if ((options.value("proxyjump", "none") != "none") ||
                        (options.value("proxycommand", "none") != "none")) {
                        state->setText(UiLanguage::tr("For jump or proxy hosts, verify the target key with "
                                                      "your existing SSH client, then reconnect."));
                        return;
                    }
                    const auto hostname = options.value("hostname", config.host);
                    actualPort = options.value("port", "22").toInt();
                    alias = options.value("hostkeyalias");
                    if (alias == "none")
                        alias.clear();
                    stage = 1;
                    state->setText(UiLanguage::tr("Reading public host keys…"));
                    process.start(keyscan, {"-T", "5", "-p", QString::number(actualPort), "--", hostname});
                    timeout.start();
                } else if (stage == 1) {
                    for (auto line : text.split('\n')) {
                        auto fields = line.simplified().split(' ');
                        if (fields.size() < 3 || fields[0].startsWith('#'))
                            continue;
                        if (!alias.isEmpty())
                            fields[0] =
                                (actualPort == 22 ? alias : "[" + alias + "]:" + QString::number(actualPort))
                                    .toUtf8();
                        keys += fields.join(' ') + '\n';
                    }
                    QFile file(temporary.filePath("hostkeys"));
                    if (keys.isEmpty() || !file.open(QIODevice::WriteOnly) ||
                        file.write(keys) != keys.size()) {
                        state->setText(UiLanguage::tr("No valid host keys were returned."));
                        return;
                    }
                    file.close();
                    stage = 2;
                    process.start(keygen, {"-l", "-E", "sha256", "-f", file.fileName()});
                    timeout.start();
                } else {
                    fingerprints->setPlainText(QString::fromUtf8(text));
                    state->setText(config.host);
                    accept->setEnabled(true);
                }
            });
    connect(&process, &QProcess::errorOccurred, &dialog, [&] {
        timeout.stop();
        state->setText(process.errorString());
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(accept, &QPushButton::clicked, &dialog, [&] {
        const auto path = RemoteSession::knownHostsPath();
        QFile previous(path);
        QByteArray contents;
        if (previous.open(QIODevice::ReadOnly))
            contents = previous.readAll();
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            state->setText(file.errorString());
            return;
        }
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        if (file.write(contents + '\n' + keys) < 0 || !file.commit()) {
            state->setText(file.errorString());
            return;
        }
        dialog.accept();
    });
    auto args = RemoteSession::arguments(config);
    args.prepend("-G");
    process.setProcessEnvironment(ToolDiscovery::environment());
    process.start(RemoteSession::sshProgram(config), args);
    timeout.start();
    dialog.resize(760, 430);
    dialog.exec();
    timeout.stop();
    process.disconnect(&dialog);
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(1000);
    }
}

void RemoteWorkspace::discoverTools() {
    if (!connected_ || files_->busy())
        return;
    QSet<QString> candidates, servers;
    for (const auto& language : ts::availableLanguages()) {
        const auto command = QProcess::splitCommand(QString::fromStdString(language.suggestedServer));
        if (!command.isEmpty()) {
            candidates.insert(command.first());
            servers.insert(command.first());
        }
        for (const auto& compiler : language.compilerCandidates)
            candidates.insert(QString::fromStdString(compiler));
        for (const auto& executable : language.sdkExecutables)
            candidates.insert(QString::fromStdString(executable));
    }
    disconnect(files_, &RemoteSession::toolsReady, this, nullptr);
    connect(files_, &RemoteSession::toolsReady, this, [this, servers](const QJsonArray& tools) {
        disconnect(files_, &RemoteSession::toolsReady, this, nullptr);
        QDialog dialog(this);
        dialog.setWindowTitle(UiLanguage::tr("Tools discovered on server"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* note = new QLabel(UiLanguage::tr("These executables come from the server PATH and registered "
                                               "language adapters. Select a tool to connect it explicitly."));
        note->setWordWrap(true);
        layout->addWidget(note);
        auto* list = new QListWidget;
        layout->addWidget(list);
        for (const auto& raw : tools) {
            const auto tool = raw.toObject();
            auto* item = new QListWidgetItem(
                tool.value("program").toString() + " · " + tool.value("path").toString(), list);
            item->setData(Qt::UserRole, tool);
        }
        auto* row = new QHBoxLayout;
        auto* useServer = new QPushButton(UiLanguage::tr("Use language server"));
        auto* useSdk = new QPushButton(UiLanguage::tr("Use SDK root"));
        auto* close = new QPushButton(UiLanguage::tr("Close"));
        row->addWidget(useServer);
        row->addWidget(useSdk);
        row->addStretch();
        row->addWidget(close);
        layout->addLayout(row);
        connect(list, &QListWidget::currentItemChanged, &dialog, [=, this](QListWidgetItem* item) {
            const auto tool = item ? item->data(Qt::UserRole).toJsonObject() : QJsonObject();
            useServer->setEnabled(!currentLanguage_.isEmpty() &&
                                  servers.contains(tool.value("program").toString()));
            useSdk->setEnabled(item);
        });
        useServer->setEnabled(false);
        useSdk->setEnabled(false);
        connect(useServer, &QPushButton::clicked, &dialog, [&] {
            const auto tool = list->currentItem()->data(Qt::UserRole).toJsonObject();
            QStringList args;
            const auto current = ts::languageForPath(currentPath().toStdString());
            const auto command = QProcess::splitCommand(QString::fromStdString(current.suggestedServer));
            if (!command.isEmpty() && command.first() == tool.value("program").toString())
                for (const auto& argument : command.mid(1))
                    args << RemoteSession::quote(argument);
            serverCommand_->setText(RemoteSession::quote(tool.value("path").toString()) +
                                    (args.isEmpty() ? QString() : " " + args.join(' ')));
            languageCommands_[currentLanguage_] = serverCommand_->text();
            startLanguageServer();
            dialog.accept();
        });
        connect(useSdk, &QPushButton::clicked, &dialog, [&] {
            auto root = list->currentItem()->data(Qt::UserRole).toJsonObject().value("path").toString();
            root = root.left(root.lastIndexOf('/'));
            if (root.endsWith("/bin"))
                root.chop(4);
            sdkRoot_->setText(root);
            sdkRoots_[currentLanguage_] = root;
            dialog.accept();
        });
        connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
        dialog.resize(780, 440);
        dialog.exec();
    });
    files_->discover(candidates.values());
}

void RemoteWorkspace::compareCurrent() {
    const auto path = currentPath();
    if (path.isEmpty() || files_->busy() || !documents_.contains(path) ||
        documents_[path].fingerprint == "missing")
        return;
    comparisonPath_ = path;
    files_->read(path);
}

void RemoteWorkspace::showComparison(const QString& path, const QByteArray& bytes,
                                     const QString& fingerprint) {
    auto found = documents_.find(path);
    if (found == documents_.end() || !found->editor)
        return;
    const QPointer<CodeEditor> original = found->editor;
    const auto revision = sourceRevision(original->document());
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString serverText = decoder(bytes);
    if (decoder.hasError() || serverText.contains(QChar(0))) {
        status_->setText(UiLanguage::tr("The remote file is not valid UTF-8 text."));
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName("remoteComparison");
    dialog.setWindowTitle(UiLanguage::tr("Compare local and server versions"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(path));
    auto* split = new QSplitter;
    const auto editorPane = [&](const QString& title, const QString& text, const QString& objectName) {
        auto* pane = new QWidget;
        auto* column = new QVBoxLayout(pane);
        column->addWidget(new QLabel(title));
        auto* editor = new CodeEditor;
        editor->setGitAnnotationsAvailable(false);
        editor->setObjectName(objectName);
        editor->setLanguage(ts::languageForPath(path.toStdString()).id);
        editor->setPlainText(text);
        editor->setReadOnly(true);
        column->addWidget(editor);
        split->addWidget(pane);
    };
    editorPane(UiLanguage::tr("Local editor"), original->toPlainText(), "comparisonLocal");
    editorPane(UiLanguage::tr("Server version"), serverText, "comparisonServer");
    layout->addWidget(split);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto* takeServer = buttons->addButton(UiLanguage::tr("Use server version"), QDialogButtonBox::ActionRole);
    auto* upload = buttons->addButton(UiLanguage::tr("Upload local version"), QDialogButtonBox::ActionRole);
    upload->setObjectName("comparisonUpload");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(takeServer, &QPushButton::clicked, &dialog, [&] { dialog.done(2); });
    connect(upload, &QPushButton::clicked, &dialog, [&] { dialog.done(3); });
    dialog.resize(1120, 720);
    const int decision = dialog.exec();
    if (decision != 2 && decision != 3)
        return;
    if (!original || sourceRevision(original->document()) != revision || !documents_.contains(path)) {
        status_->setText(
            UiLanguage::tr("The editor changed during comparison. Compare again before continuing."));
        return;
    }
    documents_[path].fingerprint = fingerprint;
    if (decision == 2) {
        documents_[path].crlf = bytes.contains("\r\n") && bytes.count('\n') == bytes.count("\r\n");
        documents_[path].utf8Bom = bytes.startsWith("\xef\xbb\xbf");
        original->setPlainText(serverText);
        original->document()->setModified(false);
        updateTabs();
    } else {
        saveQueue_ = {path};
        pumpSave();
    }
}
