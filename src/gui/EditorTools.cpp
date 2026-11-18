#include "MainWindow.hpp"
#include "UiLanguage.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDialog>
#include <QFontInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTextCursor>
#include <algorithm>

void MainWindow::goToSourceLine() {
    bool accepted = false;
    const int line =
        QInputDialog::getInt(this, UiLanguage::tr("Go to line"), UiLanguage::tr("Line:"),
                             editor_->textCursor().blockNumber() + 1, 1, editor_->blockCount(), 1, &accepted);
    if (accepted) {
        goToPage(2);
        editor_->jumpToLine(line);
    }
}

void MainWindow::configureEditorFont() {
    bool accepted = false;
    const auto font =
        QFontDialog::getFont(&accepted, editor_->font(), this, UiLanguage::tr("Editor monospaced font"),
                             QFontDialog::MonospacedFonts);
    if (!accepted)
        return;
    if (!QFontInfo(font).fixedPitch()) {
        QMessageBox::warning(this, UiLanguage::tr("Font"), UiLanguage::tr("Choose a monospaced font."));
        return;
    }
    QSettings settings;
    settings.setValue("editor/fontFamily", font.family());
    settings.setValue("editor/basePixelSize",
                      std::clamp(qRound(QFontInfo(font).pixelSize() / tsui::ThemeManager::scale()), 10, 32));
    applyUiScale(uiPercent_);
}

void MainWindow::replaceSourceText() {
    goToPage(2);
    QDialog dialog(this);
    dialog.setWindowTitle(UiLanguage::tr("Find and replace"));
    auto* form = new QFormLayout(&dialog);
    auto* find = new QLineEdit;
    auto* replacement = new QLineEdit;
    auto* sensitive = new QCheckBox(UiLanguage::tr("Match case"));
    sensitive->setChecked(true);
    form->addRow(UiLanguage::tr("Find"), find);
    form->addRow(UiLanguage::tr("Replace with"), replacement);
    form->addRow(sensitive);
    auto* next = new QPushButton(UiLanguage::tr("Find next"));
    auto* one = new QPushButton(UiLanguage::tr("Replace"));
    auto* all = new QPushButton(UiLanguage::tr("Replace all"));
    auto* status = new QLabel;
    form->addRow(next, one);
    form->addRow(all);
    form->addRow(status);
    auto search = [&] {
        if (find->text().isEmpty())
            return;
        const auto flags =
            sensitive->isChecked() ? QTextDocument::FindCaseSensitively : QTextDocument::FindFlags{};
        if (!editor_->find(find->text(), flags)) {
            auto cursor = editor_->textCursor();
            cursor.movePosition(QTextCursor::Start);
            editor_->setTextCursor(cursor);
            editor_->find(find->text(), flags);
        }
    };
    connect(next, &QPushButton::clicked, &dialog, search);
    connect(one, &QPushButton::clicked, &dialog, [&] {
        if (find->text().isEmpty())
            return;
        auto cursor = editor_->textCursor();
        if (cursor.selectedText().compare(find->text(), sensitive->isChecked() ? Qt::CaseSensitive
                                                                               : Qt::CaseInsensitive) != 0) {
            search();
            cursor = editor_->textCursor();
        }
        if (cursor.hasSelection())
            cursor.insertText(replacement->text());
    });
    connect(all, &QPushButton::clicked, &dialog, [&] {
        if (find->text().isEmpty())
            return;
        auto text = editor_->toPlainText();
        const auto before = text;
        text.replace(find->text(), replacement->text(),
                     sensitive->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive);
        if (text == before) {
            status->setText(UiLanguage::tr("No matches found."));
            return;
        }
        auto cursor = editor_->textCursor();
        cursor.beginEditBlock();
        cursor.select(QTextCursor::Document);
        cursor.insertText(text);
        cursor.endEditBlock();
        status->setText(UiLanguage::tr("Replacement complete. Revert with Undo."));
    });
    dialog.resize(470, 250);
    dialog.exec();
}

void MainWindow::configureToolchain() {
    const auto name = sourcePath_.isEmpty() ? sourceDisplayName_ : sourcePath_;
    const auto plan = ts::buildPlanFor(name.toStdString(), ".veyra/build");
    if (!plan.supported) {
        QMessageBox::information(
            this, "Toolchain",
            UiLanguage::tr(
                "This language has no single-file recipe. CMake uses its project's configuration."));
        return;
    }
    const QString prefix = "toolchains/" + QString::fromStdString(plan.language.lspId);
    QSettings settings;
    QDialog dialog(this);
    dialog.setWindowTitle(UiLanguage::tr("Compiler / interpreter · ") +
                          QString::fromStdString(plan.language.name));
    auto* form = new QFormLayout(&dialog);
    auto* explanation = new QLabel(UiLanguage::tr(
        "Enter an executable name from PATH or an absolute path without arguments. Built-in recipe flags "
        "remain unchanged; choose a compatible compiler family. CMake uses its own project toolchain."));
    explanation->setWordWrap(true);
    form->addRow(explanation);
    auto* build = new QLineEdit(
        settings.value(prefix + "/build", settings.value(prefix, QString::fromStdString(plan.build.program)))
            .toString());
    build->setEnabled(plan.build.valid);
    form->addRow("Build / Check", build);
    const bool externalRun = plan.run.program != plan.outputPath;
    auto* run = new QLineEdit(
        externalRun ? settings.value(prefix + "/run", QString::fromStdString(plan.run.program)).toString()
                    : UiLanguage::tr("Built executable"));
    run->setEnabled(externalRun);
    form->addRow("Run", run);
    auto* status = new QLabel;
    status->setWordWrap(true);
    form->addRow(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        for (const auto& pair : {qMakePair(build, plan.build.valid), qMakePair(run, externalRun)})
            if (pair.second && QStandardPaths::findExecutable(pair.first->text().trimmed()).isEmpty()) {
                status->setText(UiLanguage::tr("Tool not found: ") + pair.first->text());
                return;
            }
        if (plan.build.valid)
            settings.setValue(prefix + "/build", build->text().trimmed());
        if (externalRun)
            settings.setValue(prefix + "/run", run->text().trimmed());
        dialog.accept();
    });
    dialog.resize(680, 270);
    dialog.exec();
}
