#include "../application/BackgroundJob.hpp"
#include "../application/SourceRevision.hpp"
#include "MainWindow.hpp"
#include "SourceInspector.hpp"
#include "UiLanguage.hpp"
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QTextBlock>
#include <QTimer>
#include <memory>

void MainWindow::analyzeCode() {
    if (!editor_ || closing_)
        return;
    codeAuditTimer_->stop();
    const auto language =
        ts::languageForPath((sourcePath_.isEmpty() ? sourceDisplayName_ : sourcePath_).toStdString()).id;
    if (language != ts::LanguageId::Cpp && language != ts::LanguageId::C) {
        applyCodeAnalysis({});
        return;
    }
    if (analysisJob_->busy()) {
        analysisPending_ = true;
        analysisJob_->cancel();
        return;
    }
    analysisPending_ = false;
    const auto source = editor_->toPlainText().toStdString();
    const QPointer<QTextDocument> document = editor_->document();
    const int revision = sourceRevision(document);
    auto result = std::make_shared<ts::CodeReport>();
    analysisJob_->start(
        [source, result](const auto& cancelled, const auto&) {
            if (!cancelled())
                *result = ts::Analyzer{}.analyze(source);
        },
        [this, document, revision, result](const QString& error, bool cancelled) {
            if (closing_)
                return;
            if (error.isEmpty() && !cancelled && document && editor_->document() == document &&
                sourceRevision(document) == revision)
                applyCodeAnalysis(*result);
            if (analysisPending_)
                QTimer::singleShot(0, this, &MainWindow::analyzeCode);
        });
}

void MainWindow::applyCodeAnalysis(const ts::CodeReport& report) {
    currentCodeReport_ = report;
    editor_->setProperty("analysisCurrent", true);
    editor_->setProperty("analysisRevision", sourceRevision(editor_->document()));
    findings_->clear();
    for (const auto& hint : report.diagnostics) {
        auto* item = new QListWidgetItem(QString("%1 · %2\n%3\n%4")
                                             .arg(hint.line)
                                             .arg(UiLanguage::text(QString::fromStdString(hint.title)),
                                                  UiLanguage::text(QString::fromStdString(hint.explanation)),
                                                  UiLanguage::text(QString::fromStdString(hint.fix))),
                                         findings_);
        item->setData(Qt::UserRole, hint.line);
    }
    codeSummary_->setText(report.diagnostics.empty() ? UiLanguage::tr("No thread hints.")
                                                     : UiLanguage::tr("Thread hints: ") +
                                                           QString::number(report.diagnostics.size()));
    updateThreadHint();
    if (neuralReviewTimer_ && !threadHint_->isHidden())
        neuralReviewTimer_->start();
}

void MainWindow::updateThreadHint() {
    if (!threadHint_ || !editor_)
        return;
    if (!editor_->property("analysisCurrent").toBool() ||
        editor_->property("analysisRevision").toInt() != sourceRevision(editor_->document())) {
        threadHint_->hide();
        return;
    }
    const int line = editor_->textCursor().blockNumber() + 1;
    QStringList hints;
    for (const auto& hint : currentCodeReport_.diagnostics)
        if (hint.line == line)
            hints << UiLanguage::text(QString::fromStdString(hint.title)) + " — " +
                         UiLanguage::text(QString::fromStdString(hint.fix));
    threadHint_->setText(hints.join('\n'));
    threadHint_->setVisible(!hints.isEmpty());
}

void MainWindow::startDeepInspection() {
    const QPointer<QTextDocument> document = editor_->document();
    const int revision = sourceRevision(document);
    sourceInspector_->inspect(
        editor_->toPlainText(), sourcePath_.isEmpty() ? sourceDisplayName_ : sourcePath_,
        [this, document, revision](const InspectionResult& result) {
            if (closing_ || !document || editor_->document() != document ||
                sourceRevision(document) != revision)
                return;
            compilerIssues_.clear();
            for (const auto& issue : result.diagnostics)
                compilerIssues_.pushBack(
                    {issue.line, QString::fromStdString(issue.explanation), sourcePath_});
            updateExternalProblems();
            appendLog(result.engine + ": " + result.status + "\n" + result.output);
        });
}
