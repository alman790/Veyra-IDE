#include "../application/BackgroundJob.hpp"
#include "../application/DocumentController.hpp"
#include "../application/SourceRevision.hpp"
#include "CMakeProjectDialog.hpp"
#include "DebuggerPanel.hpp"
#include "GlassSurfaces.hpp"
#include "MainWindow.hpp"
#include "SourceInspector.hpp"
#include "UiIcons.hpp"
#include "UiLanguage.hpp"
#include "UiLayout.hpp"
#include "WorkspaceWidgets.hpp"
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringConverter>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextCursor>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace tsui::widgets;

void MainWindow::findNext() {
    const QString query = codeSearch_->text();
    if (query.isEmpty())
        return;
    if (!editor_->find(query)) {
        auto cursor = editor_->textCursor();
        cursor.movePosition(QTextCursor::Start);
        editor_->setTextCursor(cursor);
        if (!editor_->find(query))
            statusBar()->showMessage(UiLanguage::tr("Not found in source: ") + query, 4000);
    }
}

void MainWindow::syncLanguage(bool restartLsp) {
    if (!editor_)
        return;
    editor_->setFilePath(sourcePath_);
    const auto lang =
        ts::languageForPath((sourcePath_.isEmpty() ? sourceDisplayName_ : sourcePath_).toStdString());
    editor_->setLanguage(lang.id == ts::LanguageId::Unknown ? ts::LanguageId::PlainText : lang.id);
    if (activeLanguage_) {
        activeLanguage_->setText(QString::fromStdString(lang.name));
        activeLanguage_->setToolTip(UiLanguage::tr(
            "Completion: Ctrl+Space. Semantic analysis requires an installed language server."));
    }
    if (!restartLsp || !lsp_)
        return;
    lspIssues_.clear();
    updateExternalProblems();
    if (!QSettings().value("lsp/enabled", true).toBool()) {
        lsp_->stop();
        return;
    }
    lspIssues_.clear();
    lspSourcePath_ = sourcePath_;
    if (lspSourcePath_.isEmpty() && scratchDocuments_.isValid()) {
        lspSourcePath_ = editor_->document()->property("virtualLspPath").toString();
        if (lspSourcePath_.isEmpty()) {
            lspSourcePath_ =
                QDir(scratchDocuments_.path())
                    .filePath(QString::number(reinterpret_cast<quintptr>(editor_->document()), 16) + "-" +
                              QFileInfo(sourceDisplayName_).fileName());
            editor_->document()->setProperty("virtualLspPath", lspSourcePath_);
            const QPointer<LspClient> client = lsp_;
            const auto path = lspSourcePath_;
            connect(editor_->document(), &QObject::destroyed, this, [client, path] {
                if (client)
                    client->closeFile(path);
            });
        }
    } else if (!sourcePath_.isEmpty()) {
        const auto old = editor_->document()->property("virtualLspPath").toString();
        if (!old.isEmpty()) {
            lsp_->closeFile(old);
            editor_->document()->setProperty("virtualLspPath", {});
        }
    }
    lsp_->startFile(
        lspSourcePath_, editor_->toPlainText(), lang,
        [this](const QString& line) {
            appendLog(line);
            if (line.startsWith("LSP connected"))
                refreshSemanticTokens();
            if (line.startsWith("Language action failed:"))
                statusBar()->showMessage(UiLanguage::text(line), 7000);
        },
        [this](const QStringList& items) {
            if (!editor_ ||
                editor_->property("completionDocument").value<QObject*>() != editor_->document() ||
                editor_->property("completionPosition").toInt() != editor_->textCursor().position() ||
                editor_->property("completionRevision").toInt() != sourceRevision(editor_->document()))
                return;
            editor_->setCompletionWords(items, lsp_->completionEdits());
        },
        [this](const QString& message, int line, const QString& source) {
            if (line == 0) {
                lspIssues_.clear();
                QTimer::singleShot(50, this, &MainWindow::refreshSemanticTokens);
            } else
                lspIssues_.pushBack({line, "[" + source + "] " + message, sourcePath_});
            if (workbenchProblems_) {
                if (!property("externalProblemsPending").toBool()) {
                    setProperty("externalProblemsPending", true);
                    QTimer::singleShot(0, this, [this] {
                        setProperty("externalProblemsPending", false);
                        workbenchProblems_->clear();
                        updateExternalProblems();
                    });
                }
            }
        });
}

void MainWindow::updateExternalProblems() {
    if (!workbenchProblems_)
        return;
    workbenchProblems_->clear();
    ts::Array<ts::Diagnostic> highlights;
    for (const auto& issue : lspIssues_) {
        ts::Diagnostic diagnostic;
        diagnostic.line = issue.line;
        diagnostic.title = "LSP";
        diagnostic.explanation = issue.message.toStdString();
        diagnostic.severity = issue.message.contains("[error]")     ? ts::RiskLevel::Critical
                              : issue.message.contains("[warning]") ? ts::RiskLevel::Warning
                                                                    : ts::RiskLevel::Info;
        highlights.pushBack(diagnostic);
    }
    editor_->setDiagnostics(highlights);
    for (const auto& group : {&compilerIssues_, &lspIssues_})
        for (const auto& issue : *group) {
            auto* item = new QListWidgetItem(QString("Line %1 · %2").arg(issue.line).arg(issue.message));
            item->setData(Qt::UserRole, issue.line);
            item->setData(Qt::UserRole + 1, issue.file);
            item->setToolTip(issue.message);
            workbenchProblems_->addItem(item);
        }
}

void MainWindow::consumeBuildOutput(const QString& chunk) {
    if (!runConsole_)
        return;
    QTextCursor c = runConsole_->textCursor();
    c.movePosition(QTextCursor::End);
    c.insertText(chunk);
    runConsole_->setTextCursor(c);
    runConsole_->ensureCursorVisible();
    pendingBuildLine_ += chunk;
    QRegularExpression gcc(R"(^(.+?):(\d+)(?::\d+)?:\s*(?:fatal\s+)?(error|warning|note):\s*(.+)$)");
    QRegularExpression msvc(
        R"(^(.+?)\((\d+)(?:,\d+)?\)\s*:\s*(?:fatal\s+)?(error|warning|note)(?:\s+[A-Za-z]+\d+)?:\s*(.+)$)");
    QRegularExpression java(R"(^(.+?\.java):(\d+):\s*(error|warning):\s*(.+)$)");
    while (true) {
        const int newline = pendingBuildLine_.indexOf('\n');
        if (newline < 0)
            break;
        const QString line = pendingBuildLine_.left(newline).trimmed();
        pendingBuildLine_.remove(0, newline + 1);
        auto match = gcc.match(line);
        if (!match.hasMatch())
            match = msvc.match(line);
        if (!match.hasMatch())
            match = java.match(line);
        if (match.hasMatch() && compilerIssues_.size() < 500) {
            compilerIssues_.pushBack({match.captured(2).toInt(),
                                      "[Compiler " + match.captured(3) + "] " + match.captured(4),
                                      match.captured(1)});
            if (workbenchProblems_) {
                auto* item = new QListWidgetItem(
                    QString("Line %1 · [Compiler] %2").arg(match.captured(2)).arg(match.captured(4)));
                item->setData(Qt::UserRole, match.captured(2).toInt());
                item->setData(Qt::UserRole + 1, match.captured(1));
                workbenchProblems_->addItem(item);
            }
        }
    }
    if (pendingBuildLine_.size() > 32768)
        pendingBuildLine_ = pendingBuildLine_.right(2000);
}

void MainWindow::buildActive(bool execute) {
    if (runContext_ && runContext_->currentData().toBool())
        buildProject(execute);
    else
        buildSource(execute);
}

void MainWindow::buildSource(bool execute) {
    goToPage(2);
    if (!editor_ || !codeRunner_)
        return;
    if (codeRunner_->busy()) {
        QMessageBox::information(this, "Build / Run", "A process is already running; stop it first.");
        return;
    }
    if ((sourcePath_.isEmpty() || editor_->document()->isModified()) && !writeCode())
        return;
    const auto format = ts::languageForPath(sourcePath_.toStdString()).id;
    if (format == ts::LanguageId::Markdown) {
        markdownMode_->setCurrentIndex(1);
        updateMarkdown();
        if (!execute)
            startDeepInspection();
        return;
    }
    if (format == ts::LanguageId::Json && !execute) {
        startDeepInspection();
        return;
    }
    if ((format == ts::LanguageId::Html || format == ts::LanguageId::Css) && execute) {
        auto page = sourcePath_;
        if (format == ts::LanguageId::Css) {
            page = QFileInfo(sourcePath_).dir().filePath("index.html");
            if (!QFileInfo::exists(page)) {
                QMessageBox::information(
                    this, "CSS",
                    UiLanguage::tr(
                        "Open the HTML page that uses this stylesheet. CSS is not a standalone program."));
                return;
            }
        }
        if (!trustedToRun_ &&
            QMessageBox::warning(
                this, UiLanguage::tr("Open web page"),
                UiLanguage::tr("This page may execute JavaScript in your browser. Open trusted files only."),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        trustedToRun_ = true;
        QDesktopServices::openUrl(QUrl::fromLocalFile(page));
        return;
    }
    if ((format == ts::LanguageId::Html || format == ts::LanguageId::Css ||
         format == ts::LanguageId::PlainText) &&
        !execute) {
        startDeepInspection();
        return;
    }
    const auto plan = ts::buildPlanFor(
        sourcePath_.toStdString(), (QFileInfo(sourcePath_).absolutePath() + "/.veyra/build").toStdString());
    if (!plan.supported) {
        QMessageBox::information(this, "Toolchain unavailable",
                                 "No single-file compiler/runner for " +
                                     QString::fromStdString(plan.language.name) +
                                     ". You can still edit and use an installed language server.");
        return;
    }
    if (execute && !trustedToRun_) {
        const auto confirmation = QMessageBox::warning(
            this, "Executing local code",
            "Run only code you trust. The process will have your user account's filesystem permissions. "
            "Veyra does not sandbox user programs. Continue?",
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (confirmation != QMessageBox::Yes)
            return;
        trustedToRun_ = true;
    }
    if (outputDock_)
        outputDock_->show();
    if (runConsole_)
        runConsole_->clear();
    compilerIssues_.clear();
    pendingBuildLine_.clear();
    analyzeCode();
    QString error;
    if (!codeRunner_->start(
            sourcePath_, execute, [this](const QString& text) { consumeBuildOutput(text); },
            [this](bool success, int code) {
                if (sourceStop_)
                    sourceStop_->setEnabled(false);
                if (sourceRun_)
                    sourceRun_->setEnabled(true);
                if (sourceBuild_)
                    sourceBuild_->setEnabled(true);
                statusBar()->showMessage(success ? "Build / run successful"
                                                 : QString("Build / run stopped (exit %1)").arg(code),
                                         6000);
                analyzeCode();
                appendLog(success ? "Code task completed" : "Code task failed");
            },
            error)) {
        QMessageBox::warning(this, "Build / Run", error);
        appendLog("Build failed to start: " + error);
        return;
    }
    sourceStop_->setEnabled(true);
    sourceRun_->setEnabled(false);
    sourceBuild_->setEnabled(false);
    statusBar()->showMessage(execute ? "Compiling and running current source"
                                     : "Checking/building current source");
}

void MainWindow::buildProject(bool execute) {
    goToPage(2);
    if (codeRunner_->busy()) {
        QMessageBox::information(this, "CMake", UiLanguage::tr("Stop the current program first."));
        return;
    }
    saveAllDocuments();
    if (documents_)
        for (int index = 0; index < documents_->count(); ++index)
            if (documents_->document(index)->text->isModified())
                return;
    const auto source = workspaceRoot_.isEmpty() ? sourcePath_ : workspaceRoot_;
    const auto root = ts::findCMakeRoot(source.toStdString());
    if (root.empty()) {
        QMessageBox::information(
            this, "CMake",
            UiLanguage::tr("Open a directory containing CMakeLists.txt or a file from that project."));
        return;
    }
    if (QMessageBox::warning(this, UiLanguage::tr("Trusted CMake project"),
                             UiLanguage::tr("Configure and Build can execute project commands with access to "
                                            "your account's files. Continue?"),
                             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    if (outputDock_)
        outputDock_->show();
    if (runConsole_)
        runConsole_->clear();
    compilerIssues_.clear();
    pendingBuildLine_.clear();
    CMakeProjectDialog dialog(
        QString::fromStdString(root), [this](const QString& text) { consumeBuildOutput(text); }, this,
        execute ? ProjectAction::Run : ProjectAction::Build);
    dialog.exec();
    syncLanguage(true);
}

void MainWindow::stopSource() {
    if (debugger_)
        debugger_->stop();
    if (codeRunner_)
        codeRunner_->stop();
}

void MainWindow::configureLanguageServer() {
    if (!editor_)
        return;
    const auto lang =
        ts::languageForPath((sourcePath_.isEmpty() ? sourceDisplayName_ : sourcePath_).toStdString());
    const QString id = QString::fromStdString(lang.lspId);
    QSettings settings;
    const QString key = "lsp/" + id + "/command";
    bool ok = false;
    const auto command = QInputDialog::getText(
        this, "Language server · " + QString::fromStdString(lang.name),
        "Full local LSP command (program and arguments):", QLineEdit::Normal,
        settings.value(key, QString::fromStdString(lang.suggestedServer)).toString(), &ok);
    if (!ok)
        return;
    settings.setValue(key, command.trimmed());
    syncLanguage(true);
    QMessageBox::information(
        this, "Language Server",
        "The server must be installed locally and accessible on PATH. "
        "Language support depends on that server, not on the number of file extensions.");
}
