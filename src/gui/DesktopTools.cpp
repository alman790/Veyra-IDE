#include "../application/SourceRevision.hpp"
#include "GitPanel.hpp"
#include "GlassSurfaces.hpp"
#include "Keymap.hpp"
#include "MainWindow.hpp"
#include "MarkdownPreview.hpp"
#include "SettingsDialog.hpp"
#include "SourceInspector.hpp"
#include "UiLanguage.hpp"
#include <QComboBox>
#include <QDockWidget>
#include <QFileInfo>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <algorithm>

void MainWindow::showSettings() {
    SettingsDialog dialog(this, [this] { applyPreferences(); });
    dialog.exec();
}

void MainWindow::applyPreferences() {
    for (auto* shortcut : findChildren<QShortcut*>())
        if (shortcut->property("keymapId").isValid())
            shortcut->setKey(
                QKeySequence(shortcutKey(shortcut->property("keymapId").toByteArray().constData())));
    for (auto* action : findChildren<QAction*>())
        if (action->property("keymapId").isValid())
            action->setShortcut(
                QKeySequence(shortcutKey(action->property("keymapId").toByteArray().constData())));
    QSettings prefs;
    UiLanguage::setLanguage(prefs.value("ui/language", "en").toString());
    applyTheme(tsui::ThemeManager::fromKey(prefs.value("appearance/theme", "darcula").toString()));
    setGlassEnabled(prefs.value("appearance/glass", true).toBool());
    applyUiScale(prefs.value("appearance/uiScale", 100).toInt());
    editor_->applyPreferences();
    if (sourceBuild_)
        sourceBuild_->setToolTip(UiLanguage::tr("Build selected configuration · ") + shortcutKey("build"));
    if (sourceRun_)
        sourceRun_->setToolTip(UiLanguage::tr("Run selected configuration · ") + shortcutKey("run"));
    if (sourceStop_)
        sourceStop_->setToolTip(UiLanguage::tr("Stop program · ") + shortcutKey("stop"));
    if (auto* files = findChild<QToolButton*>("railProject"))
        files->setToolTip(UiLanguage::tr("Project files · ") + shortcutKey("project"));
    if (codeAuditTimer_) {
        codeAuditTimer_->setInterval(prefs.value("threads/delay", 250).toInt());
        if (!prefs.value("threads/live", true).toBool())
            codeAuditTimer_->stop();
    }
    if (auto* git = findChild<QToolButton*>("railGit"))
        git->setVisible(prefs.value("git/enabled", true).toBool() && gitPanel_ && gitPanel_->hasRepository());
    if (gitDock_ && !prefs.value("git/enabled", true).toBool())
        gitDock_->hide();
    if (lsp_) {
        if (!prefs.value("lsp/enabled", true).toBool())
            lsp_->stop();
        else
            syncLanguage(true);
    }
    updateMarkdown(true);
}

void MainWindow::updateMarkdown(bool changedDocument) {
    if (!markdownPreview_ || !editor_)
        return;
    const auto suffix =
        QFileInfo(sourcePath_.isEmpty() ? sourceDisplayName_ : sourcePath_).suffix().toLower();
    const bool markdown = suffix == "md" || suffix == "markdown";
    markdownMode_->setVisible(markdown);
    if (changedDocument) {
        QSignalBlocker block(markdownMode_);
        markdownMode_->setCurrentIndex(std::clamp(QSettings().value("markdown/mode", 1).toInt(), 0, 2));
    }
    const int mode = markdownMode_->currentIndex();
    editor_->setVisible(!markdown || mode != 2);
    markdownPreview_->setVisible(markdown && mode != 0);
    if (!markdown)
        return;
    const int scroll = markdownPreview_->verticalScrollBar()->value();
    markdownPreview_->render(editor_->toPlainText(), sourcePath_);
    markdownPreview_->verticalScrollBar()->setValue(scroll);
    if (changedDocument)
        documentSplit_->setSizes({600, 600});
}

void MainWindow::requestCodeCompletion(int attempt) {
    if (!lsp_ || !editor_ || lspSourcePath_.isEmpty() || attempt >= 50)
        return;
    if (!lsp_->ready()) {
        if (!QSettings().value("lsp/enabled", true).toBool())
            return;
        const QPointer<QTextDocument> document = editor_->document();
        const int position = editor_->textCursor().position();
        QTimer::singleShot(200, this, [this, document, position, attempt] {
            if (document && editor_->document() == document && editor_->textCursor().position() == position)
                requestCodeCompletion(attempt + 1);
        });
        return;
    }
    lspChangeTimer_->stop();
    lsp_->changeText(editor_->toPlainText());
    const auto cursor = editor_->textCursor();
    editor_->setProperty("completionDocument", QVariant::fromValue<QObject*>(editor_->document()));
    editor_->setProperty("completionPosition", cursor.position());
    editor_->setProperty("completionRevision", sourceRevision(editor_->document()));
    lsp_->requestCompletion(cursor.blockNumber(), cursor.positionInBlock());
}
