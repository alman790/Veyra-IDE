#include "../application/DocumentController.hpp"
#include "../application/SourceRevision.hpp"
#include "MainWindow.hpp"
#include "UiLanguage.hpp"
#include <QDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QSettings>
#include <QStatusBar>
#include <QTextBlock>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <vector>

bool MainWindow::prepareLanguageRequest() {
    if (!documents_ || !documents_->activeDocument() || !lsp_ || !lsp_->ready()) {
        statusBar()->showMessage(
            UiLanguage::tr("Connect a local language server for the selected SDK first."), 6000);
        return false;
    }
    lsp_->changeText(editor_->toPlainText());
    return true;
}

void MainWindow::navigateSymbol(bool references) {
    if (!prepareLanguageRequest())
        return;
    const auto cursor = editor_->textCursor();
    const QPointer<QTextDocument> origin = editor_->document();
    const int revision = sourceRevision(origin), position = cursor.position();
    const auto extra =
        references ? QJsonObject{{"context", QJsonObject{{"includeDeclaration", true}}}} : QJsonObject{};
    lsp_->requestDocument(
        references ? "textDocument/references" : "textDocument/definition", cursor.blockNumber(),
        cursor.positionInBlock(), extra,
        [this, references, origin, revision, position](const QJsonValue& result) {
            if (!origin || editor_->document() != origin || sourceRevision(origin) != revision ||
                editor_->textCursor().position() != position)
                return;
            const auto locations = result.isArray() ? result.toArray() : QJsonArray{result};
            struct Location {
                QString file;
                int line;
                int column;
            };
            QList<Location> targets;
            for (const auto& value : locations) {
                const auto object = value.toObject();
                const auto uri = QUrl(object.value("uri").toString(object.value("targetUri").toString()));
                if (!uri.isLocalFile())
                    continue;
                auto range = object.value("range").toObject();
                if (range.isEmpty())
                    range = object.value("targetSelectionRange").toObject();
                const auto start = range.value("start").toObject();
                targets.push_back(
                    {uri.toLocalFile(), start.value("line").toInt(), start.value("character").toInt()});
            }
            const auto activate = [this](const Location& target) {
                if (!loadCodeFile(target.file))
                    return;
                const auto block = editor_->document()->findBlockByNumber(target.line);
                if (!block.isValid())
                    return;
                editor_->invalidateViewRestore();
                QTextCursor cursor(block);
                cursor.setPosition(block.position() + std::min(target.column, int(block.text().size())));
                editor_->setTextCursor(cursor);
                editor_->centerCursor();
            };
            if (targets.isEmpty()) {
                statusBar()->showMessage(UiLanguage::tr("No locations were returned by the language server."),
                                         5000);
                return;
            }
            if (targets.size() == 1 && !references) {
                activate(targets[0]);
                return;
            }
            QDialog dialog(this);
            dialog.setWindowTitle(references ? UiLanguage::tr("Usages") : UiLanguage::tr("Definitions"));
            auto* layout = new QVBoxLayout(&dialog);
            auto* list = new QListWidget;
            layout->addWidget(list);
            for (const auto& target : targets)
                list->addItem(
                    QString("%1:%2:%3").arg(target.file).arg(target.line + 1).arg(target.column + 1));
            connect(list, &QListWidget::itemActivated, &dialog, [&](QListWidgetItem* item) {
                activate(targets[list->row(item)]);
                dialog.accept();
            });
            dialog.resize(750, 380);
            dialog.exec();
        });
}

void MainWindow::renameSymbol() {
    if (!prepareLanguageRequest())
        return;
    saveAllDocuments();
    for (int index = 0; index < documents_->count(); ++index)
        if (documents_->document(index)->text->isModified())
            return;
    if (!prepareLanguageRequest())
        return;
    auto cursor = editor_->textCursor();
    auto selection = cursor;
    selection.select(QTextCursor::WordUnderCursor);
    bool accepted = false;
    const auto name =
        QInputDialog::getText(this, UiLanguage::tr("Rename symbol"), UiLanguage::tr("New name:"),
                              QLineEdit::Normal, selection.selectedText(), &accepted)
            .trimmed();
    if (!accepted || name.isEmpty() || name.contains('\n'))
        return;
    lsp_->requestDocument("textDocument/rename", cursor.blockNumber(), cursor.positionInBlock(),
                          {{"newName", name}},
                          [this](const QJsonValue& result) { applyLanguageEdits(result.toObject()); });
}

void MainWindow::formatDocument() {
    if (!prepareLanguageRequest())
        return;
    lsp_->requestDocument(
        "textDocument/formatting", -1, -1,
        {{"options",
          QJsonObject{{"tabSize", QSettings().value("editor/tabWidth", 4).toInt()}, {"insertSpaces", true}}}},
        [this](const QJsonValue& result) {
            applyLanguageEdits(
                {{"changes",
                  QJsonObject{{QUrl::fromLocalFile(lspSourcePath_).toString(QUrl::FullyEncoded), result}}}});
        });
}

bool MainWindow::applyLanguageEdits(const QJsonObject& workspace) {
    QHash<QString, QJsonArray> changes;
    const auto simple = workspace.value("changes").toObject();
    for (auto it = simple.begin(); it != simple.end(); ++it)
        changes[it.key()] = it.value().toArray();
    for (const auto& value : workspace.value("documentChanges").toArray()) {
        const auto change = value.toObject();
        if (change.contains("kind")) {
            statusBar()->showMessage(
                UiLanguage::tr("The server requested a file operation; no edits were applied."), 6000);
            return false;
        }
        const auto document = change.value("textDocument").toObject();
        const auto uri = document.value("uri").toString();
        if (document.value("version").isDouble() &&
            lsp_->documentVersion(QUrl(uri).toLocalFile()) != document.value("version").toInt()) {
            statusBar()->showMessage(UiLanguage::tr("The document changed; request this action again."),
                                     6000);
            return false;
        }
        changes[uri] = change.value("edits").toArray();
    }

    struct Edit {
        int start;
        int end;
        QString text;
    };

    struct Prepared {
        QString path;
        QString content;
        QTextDocument* document;
        std::vector<Edit> edits;
    };

    std::vector<Prepared> prepared;
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        const QUrl url(it.key());
        if (!url.isLocalFile())
            return false;
        const auto path = QFileInfo(url.toLocalFile()).absoluteFilePath();
        QTextDocument* document = nullptr;
        for (int index = 0; index < documents_->count(); ++index) {
            const auto* open = documents_->document(index);
            if (open->path == path || (open == documents_->activeDocument() && path == lspSourcePath_))
                document = open->text.get();
        }
        QString content, error;
        if (document)
            content = document->toPlainText();
        else if (!DocumentController::readUtf8(path, content, error))
            return false;
        QTextDocument snapshot;
        snapshot.setPlainText(content);
        Prepared item{path, content, document, {}};
        for (const auto& value : it.value()) {
            const auto edit = value.toObject();
            const auto range = edit.value("range").toObject();
            const auto position = [&](const QJsonObject& point) {
                const int line = point.value("line").toInt(-1), column = point.value("character").toInt(-1);
                const auto block = snapshot.findBlockByNumber(line);
                if (!block.isValid() || column < 0 || column > block.text().size())
                    return -1;
                return block.position() + column;
            };
            const int start = position(range.value("start").toObject()),
                      end = position(range.value("end").toObject());
            if (start < 0 || end < start)
                return false;
            item.edits.push_back({start, end, edit.value("newText").toString()});
        }
        std::sort(item.edits.begin(), item.edits.end(),
                  [](const auto& left, const auto& right) { return left.start > right.start; });
        int previous = content.size() + 1;
        for (const auto& edit : item.edits) {
            if (edit.end > previous)
                return false;
            previous = edit.start;
        }
        prepared.push_back(std::move(item));
    }
    // Validate every range and load every buffer before mutating any source text.
    const int active = documents_->activeIndex();
    for (auto& item : prepared) {
        if (!item.document) {
            QString error;
            if (!documents_->open(item.path, error))
                return false;
            item.document = documents_->activeDocument()->text.get();
        }
    }
    for (auto& item : prepared) {
        QTextCursor cursor(item.document);
        cursor.beginEditBlock();
        for (const auto& edit : item.edits) {
            cursor.setPosition(edit.start);
            cursor.setPosition(edit.end, QTextCursor::KeepAnchor);
            cursor.insertText(edit.text);
        }
        cursor.endEditBlock();
    }
    documents_->activate(active);
    statusBar()->showMessage(UiLanguage::tr("Edits applied to open buffers. Save All writes them to disk."),
                             5000);
    return true;
}

void MainWindow::showSymbolInfo() {
    if (!prepareLanguageRequest())
        return;
    const auto cursor = editor_->textCursor();
    lsp_->requestDocument(
        "textDocument/hover", cursor.blockNumber(), cursor.positionInBlock(), {},
        [this](const QJsonValue& result) {
            const auto contents = result.toObject().value("contents");
            QString text;
            if (contents.isString())
                text = contents.toString();
            else if (contents.isObject())
                text = contents.toObject().value("value").toString();
            else
                for (const auto& value : contents.toArray())
                    text += value.isString() ? value.toString() : value.toObject().value("value").toString();
            if (!text.isEmpty())
                QToolTip::showText(editor_->viewport()->mapToGlobal(editor_->cursorRect().bottomLeft()),
                                   text.toHtmlEscaped().replace('\n', "<br>"), editor_);
        });
}

void MainWindow::refreshSemanticTokens() {
    if (!lsp_ || !lsp_->ready() || lsp_->semanticTypes().isEmpty() || !documents_ ||
        !documents_->activeDocument())
        return;
    const QPointer<QTextDocument> document = editor_->document();
    const int revision = sourceRevision(document);
    const auto legend = lsp_->semanticTypes();
    lsp_->requestDocument(
        "textDocument/semanticTokens/full", -1, -1, {},
        [this, document, revision, legend](const QJsonValue& result) {
            if (document && editor_->document() == document && sourceRevision(document) == revision)
                editor_->setSemanticTokens(result.toObject().value("data").toArray(), legend);
        });
}
