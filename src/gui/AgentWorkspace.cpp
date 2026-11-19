#include "../application/DocumentController.hpp"
#include "AgentPanel.hpp"
#include "MainWindow.hpp"
#include "ToolDiscovery.hpp"
#include "UiLanguage.hpp"
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QTextCursor>
#include <algorithm>

void MainWindow::setupProjectAgent() {
    agentDock_ = new QDockWidget(UiLanguage::tr("Project agent"), this);
    agentDock_->setObjectName("projectAgentDock");
    agentDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    agentPanel_ = new AgentPanel(agentDock_);
    agentDock_->setWidget(agentPanel_);
    addDockWidget(Qt::RightDockWidgetArea, agentDock_);
    agentDock_->hide();
    connect(
        agentPanel_, &AgentPanel::requestSubmitted, this,
        [this](const QString& question, const QString& provider, const QString& model, bool proposeChanges) {
            auto* document = documents_ ? documents_->activeDocument() : nullptr;
            if (!document || document->path.isEmpty()) {
                agentPanel_->reportError(
                    UiLanguage::tr("Open a saved file inside the project before starting the agent."));
                return;
            }
            AgentInput input;
            input.requestChanges = proposeChanges;
            input.root = workspaceRoot_.isEmpty() ? QFileInfo(document->path).absolutePath() : workspaceRoot_;
            input.root = QDir(input.root).canonicalPath();
            input.path = document->path;
            input.text = document->text->toPlainText();
            input.cursor = editor_->textCursor().position();
            input.selectionStart = editor_->textCursor().selectionStart();
            input.selectionEnd = editor_->textCursor().selectionEnd();
            const auto language = ts::languageForPath(document->path.toStdString());
            input.language = QString::fromStdString(language.name);
            for (const auto& file : language.projectFiles)
                if (file.first != language.entry)
                    input.configurationFiles.append(QString::fromStdString(file.first));
            const auto sdk = ToolDiscovery::projectSdk(input.root);
            input.sdk = sdk.root.isEmpty()
                            ? UiLanguage::tr("No project SDK selected")
                            : (sdk.name.isEmpty() ? QFileInfo(sdk.root).fileName() : sdk.name) + " (" +
                                  sdk.root + ')';
            if (sdk.root.isEmpty()) {
                QStringList available;
                for (const auto& global : ToolDiscovery::sdks())
                    available << global.name + " (" + global.root + ')';
                if (!available.isEmpty())
                    input.sdk = available.join(", ");
            }
            if (runConsole_)
                input.buildOutput = runConsole_->toPlainText().right(6000);
            for (int index = 0; index < documents_->count(); ++index) {
                const auto* open = documents_->document(index);
                if (open->path.isEmpty())
                    continue;
                const auto relative = QDir(input.root).relativeFilePath(open->path);
                if (!open->path.startsWith(input.root + '/') || !AgentContext::allowedPath(relative))
                    continue;
                if (open == document)
                    input.openFiles[open->path] = input.text;
                else if (input.openFiles.size() < 15 && open->text->characterCount() <= 1024 * 1024)
                    input.openFiles[open->path] = open->text->toPlainText();
                else
                    input.unavailableBuffers.append(relative);
            }
            const auto appendIssues = [&](const auto& issues, const QString& source) {
                for (const auto& issue : issues) {
                    if (input.diagnostics.size() >= 50)
                        break;
                    input.diagnostics.append(QJsonObject{
                        {"file",
                         QDir(input.root).relativeFilePath(issue.file.isEmpty() ? input.path : issue.file)},
                        {"line", issue.line},
                        {"message", issue.message.left(1000)},
                        {"source", source}});
                }
            };
            appendIssues(compilerIssues_, "compiler");
            appendIssues(lspIssues_, "language server");
            agentPanel_->agent()->start(input, question, provider, model);
        });
    connect(agentPanel_, &AgentPanel::applyRequested, this, &MainWindow::applyAgentProposal);
    connect(agentPanel_, &AgentPanel::buildRequested, this, [this] { buildActive(false); });
    connect(agentPanel_->agent(), &ProjectAgent::busyChanged, this, [this](bool busy) {
        if (busy && agentPanel_->agent()->usesLocalInference()) {
            ++localGeneration_;
            if (predictiveTimer_)
                predictiveTimer_->stop();
            if (neuralReviewTimer_)
                neuralReviewTimer_->stop();
            if (localModelJob_)
                localModelJob_->cancel();
            editor_->setGhostText({});
        }
    });
    connect(editor_, &QPlainTextEdit::textChanged, agentPanel_, &AgentPanel::invalidateProposal);
}

void MainWindow::applyAgentProposal() {
    if (!agentPanel_ || !documents_ || agentPanel_->agent()->busy())
        return;
    const auto proposal = agentPanel_->proposal();
    const auto context = agentPanel_->agent()->context();
    if (!context || proposal.edits.isEmpty())
        return;
    if (!workspaceRoot_.isEmpty() && QDir(workspaceRoot_).canonicalPath() != context->root()) {
        agentPanel_->invalidateProposal();
        agentPanel_->reportError(
            UiLanguage::tr("The active project changed. Request a new proposal before applying edits."));
        return;
    }

    struct Prepared {
        QString path;
        QTextDocument* document = nullptr;
        QList<AgentEdit> edits;
    };

    QMap<QString, Prepared> prepared;
    for (const auto& edit : proposal.edits) {
        const auto absolute = QFileInfo(QDir(context->root()).filePath(edit.path)).canonicalFilePath();
        if (!AgentContext::allowedPath(edit.path) || absolute.isEmpty() ||
            !absolute.startsWith(context->root() + '/') ||
            QDir(context->root()).relativeFilePath(absolute) != edit.path) {
            agentPanel_->reportError(
                UiLanguage::tr("A proposed file is no longer inside the project. No edits were applied."));
            return;
        }
        auto& item = prepared[edit.path];
        item.path = absolute;
        item.edits.append(edit);
    }
    int unopened = 0;
    for (auto it = prepared.begin(); it != prepared.end(); ++it) {
        for (int index = 0; index < documents_->count(); ++index)
            if (documents_->document(index)->path == it->path)
                it->document = documents_->document(index)->text.get();
        QString current, error;
        if (it->document)
            current = it->document->toPlainText();
        else {
            ++unopened;
            if (!DocumentController::readUtf8(it->path, current, error)) {
                agentPanel_->reportError(UiLanguage::text(error));
                return;
            }
        }
        if (!context->snapshots().contains(it.key()) || current != context->snapshots().value(it.key())) {
            agentPanel_->invalidateProposal();
            agentPanel_->reportError(UiLanguage::tr("Source changed after this proposal was generated. "
                                                    "Request a fresh proposal; no edits were applied."));
            return;
        }
        std::sort(it->edits.begin(), it->edits.end(),
                  [](const auto& a, const auto& b) { return a.offset > b.offset; });
    }
    if (documents_->count() + unopened > 50) {
        agentPanel_->reportError(UiLanguage::tr("Close some document tabs before applying this proposal."));
        return;
    }
    const int active = documents_->activeIndex();
    const QPointer<QTextDocument> originalDocument = editor_->document();
    const auto originalCursor = editor_->textCursor();
    for (auto& item : prepared) {
        if (!item.document) {
            QString error;
            if (!documents_->open(item.path, error)) {
                agentPanel_->reportError(UiLanguage::text(error));
                return;
            }
            item.document = documents_->activeDocument()->text.get();
        }
    }
    for (auto& item : prepared) {
        QTextCursor cursor(item.document);
        cursor.beginEditBlock();
        for (const auto& edit : item.edits) {
            cursor.setPosition(edit.offset);
            cursor.setPosition(edit.offset + edit.before.size(), QTextCursor::KeepAnchor);
            cursor.insertText(edit.after);
        }
        cursor.endEditBlock();
    }
    documents_->activate(active);
    if (originalDocument && editor_->document() == originalDocument) {
        editor_->invalidateViewRestore();
        editor_->setTextCursor(originalCursor);
    }
    agentPanel_->applied();
}
