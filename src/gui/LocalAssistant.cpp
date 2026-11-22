#include "../application/BackgroundJob.hpp"
#include "../application/SourceRevision.hpp"
#include "AgentPanel.hpp"
#include "MainWindow.hpp"
#include "UiLanguage.hpp"
#if TS_HAS_LOCAL_MODEL
#include "ts/LocalCodeModel.hpp"
#endif
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QPointer>
#include <QSettings>
#include <QShortcut>
#include <QStatusBar>
#include <QTextBlock>
#include <QTimer>
#include <memory>

void MainWindow::setupLocalAssistant() {
    localModelJob_ = new BackgroundJob(this);
    predictiveTimer_ = new QTimer(this);
    predictiveTimer_->setSingleShot(true);
    predictiveTimer_->setInterval(450);
    neuralReviewTimer_ = new QTimer(this);
    neuralReviewTimer_->setSingleShot(true);
    neuralReviewTimer_->setInterval(2400);
    connect(neuralReviewTimer_, &QTimer::timeout, this, [this] { reviewThreadWithModel(false); });
    const auto directory = QCoreApplication::applicationDirPath();
    for (const auto& path : {QDir(directory).filePath("../Resources/models/code-model.gguf"),
                             QDir(directory).filePath("models/code-model.gguf"),
                             QDir(directory).filePath("../share/VeyraIDE/models/code-model.gguf")})
        if (QFileInfo::exists(path) && qEnvironmentVariableIsEmpty("TS_CODE_MODEL"))
            qputenv("TS_CODE_MODEL", QFileInfo(path).absoluteFilePath().toUtf8());
    connect(predictiveTimer_, &QTimer::timeout, this, &MainWindow::requestPredictiveCompletion);
    connect(editor_, &CodeEditor::predictiveCompletionRequested, this, [this] {
        if (agentPanel_ && agentPanel_->agent()->usesLocalInference())
            return;
        if (!qEnvironmentVariableIsSet("TS_DISABLE_LOCAL_MODEL") &&
            QSettings().value("assistant/completion", true).toBool())
            predictiveTimer_->start();
    });
    const auto cancel = [this] {
        ++localGeneration_;
        predictiveTimer_->stop();
        neuralReviewTimer_->stop();
        localModelJob_->cancel();
        editor_->setGhostText({});
    };
    connect(editor_, &CodeEditor::assistantDismissed, this, cancel);
    connect(editor_, &CodeEditor::sourceTextChanged, this, cancel);
    connect(editor_, &QPlainTextEdit::cursorPositionChanged, this, cancel);
    connect(new QShortcut(QKeySequence("Alt+\\"), this), &QShortcut::activated, this,
            &MainWindow::requestPredictiveCompletion);
}

void MainWindow::requestPredictiveCompletion() {
#if TS_HAS_LOCAL_MODEL
    if (closing_ || (agentPanel_ && agentPanel_->agent()->usesLocalInference()) || editor_->isReadOnly() ||
        editor_->textCursor().hasSelection() || qEnvironmentVariableIsSet("TS_DISABLE_LOCAL_MODEL"))
        return;
    if (localModelJob_->busy()) {
        localModelJob_->cancel();
        predictiveTimer_->start(200);
        return;
    }
    const QPointer<QTextDocument> document = editor_->document();
    const auto cursor = editor_->textCursor();
    const int position = cursor.position(), revision = sourceRevision(document);
    const auto generation = ++localGeneration_;
    auto context = cursor;
    context.setPosition(std::max(0, position - 2000));
    context.setPosition(position, QTextCursor::KeepAnchor);
    const auto before = context.selectedText().replace(QChar::ParagraphSeparator, '\n').toStdString();
    context.setPosition(position);
    context.setPosition(std::min(document->characterCount() - 1, position + 350), QTextCursor::KeepAnchor);
    const auto after = context.selectedText().replace(QChar::ParagraphSeparator, '\n').toStdString();
    const auto& profile =
        ts::languageForPath((sourcePath_.isEmpty() ? sourceDisplayName_ : sourcePath_).toStdString());
    const auto language = profile.name;
    const auto symbols = editor_->property("semanticCandidates")
                             .toStringList()
                             .mid(0, 20)
                             .join(", ")
                             .left(700)
                             .replace('\n', ' ')
                             .replace('\r', ' ');
    const auto sdkPrelude =
        symbols.isEmpty() || profile.lineComment.empty()
            ? std::string()
            : profile.lineComment + " Available SDK symbols: " + symbols.toStdString() + "\n";
    auto result = std::make_shared<std::string>();
    localModelJob_->start(
        [before, after, language, sdkPrelude, result](const auto& cancelled, const auto&) {
            *result = ts::LocalCodeModel::complete(before, after, language, cancelled, sdkPrelude);
        },
        [this, document, position, revision, generation, result](const QString& error, bool cancelled) {
            if (closing_ || cancelled || !document || generation != localGeneration_ ||
                document != editor_->document() || sourceRevision(document) != revision ||
                editor_->textCursor().position() != position)
                return;
            if (!error.isEmpty()) {
                statusBar()->showMessage(UiLanguage::text(error), 6000);
                return;
            }
            editor_->setGhostText(QString::fromStdString(*result));
        });
#endif
}

void MainWindow::reviewThreadWithModel(bool interactive) {
#if !TS_HAS_LOCAL_MODEL
    Q_UNUSED(interactive);
#endif
#if TS_HAS_LOCAL_MODEL
    if (closing_ || (agentPanel_ && agentPanel_->agent()->usesLocalInference()) || editor_->isReadOnly() ||
        qEnvironmentVariableIsSet("TS_DISABLE_LOCAL_MODEL") ||
        !QSettings().value("assistant/review", true).toBool())
        return;
    if (localModelJob_->busy()) {
        if (!interactive) {
            neuralReviewTimer_->start(750);
            return;
        }
        localModelJob_->cancel();
        const auto generation = localGeneration_;
        QTimer::singleShot(100, this, [this, generation] {
            if (generation == localGeneration_)
                reviewThreadWithModel(true);
        });
        return;
    }
    const QPointer<QTextDocument> document = editor_->document();
    const int revision = sourceRevision(document);
    const auto generation = ++localGeneration_;
    const int position = editor_->textCursor().position();
    auto context = editor_->textCursor();
    context.setPosition(std::max(0, position - 1600));
    context.setPosition(std::min(document->characterCount() - 1, position + 1600), QTextCursor::KeepAnchor);
    const auto source = context.selectedText().replace(QChar::ParagraphSeparator, '\n').toStdString();
    std::vector<std::string> candidates;
    QStringList labels;
    const int line = editor_->textCursor().blockNumber() + 1;
    for (const auto& hint : currentCodeReport_.diagnostics) {
        if (std::abs(hint.line - line) > 2 || labels.size() >= 6)
            continue;
        const auto label = QString::fromStdString(hint.fix);
        if (!labels.contains(label)) {
            labels << label;
            candidates.push_back(hint.explanation + " Recommendation: " + hint.fix);
        }
    }
    if (candidates.empty()) {
        labels << "Check which data is shared between workers and how each access is synchronized."
               << "Check thread and task lifetimes before changing synchronization."
               << "Inspect cancellation and blocking calls in the selected SDK documentation.";
        for (const auto& label : labels)
            candidates.push_back(label.toStdString());
    }
    threadHint_->setText(UiLanguage::tr("Reviewing locally…"));
    threadHint_->show();
    auto result = std::make_shared<int>(-1);
    localModelJob_->start(
        [source, candidates, result](const auto& cancelled, const auto&) {
            *result = ts::LocalCodeModel::choose(source, candidates, cancelled);
        },
        [this, document, revision, generation, result, labels](const QString& error, bool cancelled) {
            if (closing_ || cancelled || !document || document != editor_->document() ||
                sourceRevision(document) != revision || generation != localGeneration_)
                return;
            if (!error.isEmpty()) {
                statusBar()->showMessage(UiLanguage::text(error), 6000);
                updateThreadHint();
                return;
            }
            const auto recommendation =
                *result >= 0 && *result < labels.size()
                    ? UiLanguage::text(labels[*result])
                    : UiLanguage::tr("No grounded recommendation was selected. Review the SDK diagnostics.");
            threadHint_->setText(UiLanguage::tr("Local model · ") + recommendation);
            threadHint_->show();
        });
#endif
}
