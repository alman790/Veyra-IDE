#include "../application/CMakeFileApi.hpp"
#include "../application/DocumentController.hpp"
#include "DebuggerPanel.hpp"
#include "MainWindow.hpp"
#include "UiLanguage.hpp"
#include <QCryptographicHash>
#include <QDockWidget>
#include <QFileInfo>
#include <QJsonArray>
#include <QSettings>
#include <QStatusBar>

QJsonObject MainWindow::breakpoints() const {
    QJsonObject result;
    if (!documents_)
        return result;
    for (int index = 0; index < documents_->count(); ++index) {
        const auto* document = documents_->document(index);
        if (!document->path.isEmpty())
            result[document->path] =
                QJsonArray::fromVariantList(document->text->property("breakpoints").toList());
    }
    return result;
}

void MainWindow::startDebugging() {
    if (!documents_ || !documents_->activeDocument() || codeRunner_->busy())
        return;
    if (!debugger_) {
        debugDock_ = new QDockWidget(UiLanguage::tr("Debug"), this);
        debugDock_->setObjectName("debugDock");
        debugger_ = new DebuggerPanel(debugDock_, [this](const QString& file, int line) {
            if (loadCodeFile(file)) {
                editor_->jumpToLine(line);
                editor_->setExecutionLine(line);
            }
        });
        debugDock_->setWidget(debugger_);
        addDockWidget(Qt::BottomDockWidgetArea, debugDock_);
        connect(editor_, &CodeEditor::breakpointChanged, this,
                [this] { debugger_->setBreakpoints(breakpoints()); });
    }
    debugDock_->show();
    if (debugger_->busy()) {
        debugger_->send({{"command", "continue"}});
        return;
    }
    if (!writeCode())
        return;
    saveAllDocuments();
    for (int index = 0; index < documents_->count(); ++index)
        if (documents_->document(index)->text->isModified())
            return;
    const auto source = sourcePath_;
    const auto start = [this](const QString& executable, const QString& directory) {
        QString error;
        if (!debugger_->start(executable, directory, breakpoints(), error))
            statusBar()->showMessage(UiLanguage::text(error), 8000);
    };
    QString error;
    const auto root = workspaceRoot_.isEmpty()
                          ? QString::fromStdString(ts::findCMakeRoot(source.toStdString()))
                          : workspaceRoot_;
    if (!root.isEmpty() && QFileInfo::exists(QDir(root).filePath("CMakeLists.txt"))) {
        const auto key =
            "cmake/project/" +
            QString::fromLatin1(QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha256).toHex());
        QSettings preferences;
        CMakeSettings settings{
            root, preferences.value(key + "/build", QDir(root).filePath(".veyra/cmake-debug")).toString(),
            preferences.value(key + "/generator").toString(), "Debug",
            preferences.value(key + "/target").toString()};
        if (!CMakeFileApi::prepareQuery(settings.build, error)) {
            statusBar()->showMessage(UiLanguage::text(error), 8000);
            return;
        }
        ts::Array<ts::ProcessSpec> commands;
        commands.pushBack(CMakeFileApi::configureCommand(settings));
        commands.pushBack(CMakeFileApi::buildCommand(settings, settings.target));
        if (!codeRunner_->startCommands(
                std::move(commands), false, [this](const QString& output) { consumeBuildOutput(output); },
                [this, settings, start](bool success, int) {
                    if (!success)
                        return;
                    QString error;
                    const auto targets =
                        CMakeFileApi::readTargets(settings.build, settings.configuration, error);
                    for (const auto& target : targets)
                        if (!target.artifact.isEmpty() &&
                            (settings.target.isEmpty() || settings.target == target.name)) {
                            start(target.artifact, settings.root);
                            return;
                        }
                    statusBar()->showMessage(UiLanguage::tr("No executable debug target was found."), 6000);
                },
                error))
            statusBar()->showMessage(UiLanguage::text(error), 8000);
        return;
    }
    const auto plan = ts::buildPlanFor(source.toStdString(),
                                       (QFileInfo(source).absolutePath() + "/.veyra/build").toStdString());
    if (!plan.compiled || plan.run.program != plan.outputPath) {
        statusBar()->showMessage(UiLanguage::tr("Select a native executable project for LLDB debugging."),
                                 6000);
        return;
    }
    if (!codeRunner_->start(
            source, false, [this](const QString& output) { consumeBuildOutput(output); },
            [plan, source, start](bool success, int) {
                if (success)
                    start(QString::fromStdString(plan.outputPath), QFileInfo(source).absolutePath());
            },
            error))
        statusBar()->showMessage(UiLanguage::text(error), 8000);
}
