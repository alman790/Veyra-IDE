#include "CodeRunner.hpp"
#include "SingleFileProject.hpp"
#include "ToolDiscovery.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>

CodeRunner::CodeRunner(QObject* parent) : QObject(parent) {
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    deadline_.setSingleShot(true);
    tree_.configure(process_);
    connect(&process_, &QProcess::started, this, [this] {
        QString error;
        if (!tree_.attach(process_.processId(), error) && output_)
            output_("Process-tree warning: " + error + "\n");
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, &CodeRunner::drainOutput);
    connect(&process_, &QProcess::readyReadStandardError, this, &CodeRunner::drainOutput);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError why) {
        if (!active_ || why != QProcess::FailedToStart)
            return;
        output_("Cannot start process: " + process_.errorString() + "\n");
        complete(false, -1);
    });
    connect(
        &process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this](int code, QProcess::ExitStatus status) {
            if (!active_)
                return;
            deadline_.stop();
            drainOutput();
            tree_.reset();
            if (stopping_) {
                complete(false, stopCode_);
                return;
            }
            const bool success = (code == 0 && status == QProcess::NormalExit);
            output_(
                QString("\n[%1 finished, exit %2]\n").arg(compiling_ ? "Build/check" : "Program").arg(code));
            if (!success) {
                complete(false, code);
                return;
            }
            ++stageIndex_;
            if (stageIndex_ < stages_.size()) {
                beginStage();
                return;
            }
            complete(true, code);
        });
    connect(&deadline_, &QTimer::timeout, this, [this]() {
        if (!active_)
            return;
        output_("\n[Time limit reached; process terminated]\n");
        stopping_ = true;
        stopCode_ = 124;
        tree_.terminate();
        process_.kill();
    });
}

CodeRunner::~CodeRunner() {
    output_ = {};
    finished_ = {};
    active_ = false;
    deadline_.stop();
    tree_.terminate();
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(1500);
    }
}

void CodeRunner::setTimeouts(int buildMilliseconds, int runMilliseconds) {
    buildTimeout_ = std::max(1, buildMilliseconds);
    runTimeout_ = std::max(1, runMilliseconds);
}

void CodeRunner::drainOutput() {
    const auto out = process_.readAllStandardOutput(), err = process_.readAllStandardError();
    const QString decodedOut = stdoutDecoder_(out), decodedErr = stderrDecoder_(err);
    const QString text = decodedOut + decodedErr;
    if (active_ && output_ && displayedBytes_ < 2 * 1024 * 1024)
        output_(text);
    displayedBytes_ += out.size() + err.size();
}

void CodeRunner::beginStage() {
    if (!active_ || stageIndex_ >= stages_.size())
        return;
    const auto& step = stages_[stageIndex_];
    compiling_ = !sequenceRun_ || stageIndex_ + 1 < stages_.size();
    emit stateChanged(true, !compiling_);
    QString executable = ToolDiscovery::find(QString::fromStdString(step.program));
    if (executable.isEmpty())
        executable = QString::fromStdString(step.program);
    process_.setProcessEnvironment(ToolDiscovery::environment());
    process_.setStandardInputFile(QString::fromStdString(step.standardInputFile));
    QStringList arguments;
    for (const auto& arg : step.arguments)
        arguments << QString::fromStdString(arg);
    process_.setWorkingDirectory(QString::fromStdString(step.workingDirectory));
    output_(QString("\n$ %1 %2\n").arg(executable, arguments.join(' ')));
    if (QStandardPaths::findExecutable(executable).isEmpty()) {
        if (output_)
            output_("Required tool/executable not found: " + executable + "\n");
        complete(false, 127);
        return;
    }
    stdoutDecoder_.resetState();
    stderrDecoder_.resetState();
    deadline_.start(compiling_ ? buildTimeout_ : runTimeout_);
    process_.start(executable, arguments);
}

bool CodeRunner::launch(ts::Array<ts::ProcessSpec> steps, bool execute,
                        std::function<void(const QString&)> output, std::function<void(bool, int)> finished,
                        QString& error) {
    if (active_) {
        error = "Another build/run is already active";
        return false;
    }
    if (steps.empty()) {
        error = "No build or run command available";
        return false;
    }
    for (const auto& step : steps)
        if (!step.valid || step.program.empty()) {
            error = "Build configuration is invalid. Check the selected project SDK.";
            return false;
        }
    const auto exe = QString::fromStdString(steps[0].program);
    if (ToolDiscovery::find(exe).isEmpty()) {
        error = "Required tool not installed or not on PATH: " + exe;
        return false;
    }
    stages_ = std::move(steps);
    stageIndex_ = 0;
    sequenceRun_ = execute;
    output_ = output ? std::move(output) : std::function<void(const QString&)>([](const QString&) {});
    finished_ = std::move(finished);
    stopping_ = false;
    stopCode_ = 130;
    active_ = true;
    displayedBytes_ = 0;
    beginStage();
    return true;
}

bool CodeRunner::start(const QString& sourcePath, bool execute, std::function<void(const QString&)> output,
                       std::function<void(bool, int)> finished, QString& error) {
    if (active_) {
        error = "Another build/run is already active";
        return false;
    }
    if (sourcePath.isEmpty()) {
        error = "Save the source file first";
        return false;
    }
    const QFileInfo file(sourcePath);
    if (!file.isFile()) {
        error = "Source file not found";
        return false;
    }
    const auto dir = file.absolutePath() + "/.veyra/build";
    const auto plan = ts::buildPlanFor(sourcePath.toStdString(), dir.toStdString());
    if (!plan.supported) {
        error = "No single-file toolchain for " + QString::fromStdString(plan.language.name);
        return false;
    }
    if (!execute && !plan.build.valid) {
        error = "This language has no standalone syntax-check command";
        return false;
    }
    if (!QDir().mkpath(dir)) {
        error = "Cannot create build output directory: " + dir;
        return false;
    }
    if (!prepareSingleFileProject(plan, sourcePath, dir, error))
        return false;
    ts::Array<ts::ProcessSpec> stages;
    if (plan.build.valid)
        stages.pushBack(plan.build);
    if (execute)
        stages.pushBack(plan.run);
    if (plan.build.valid) {
        const auto key = "toolchains/" + QString::fromStdString(plan.language.lspId);
        const auto override = ToolDiscovery::projectSdk().root.isEmpty()
                                  ? QSettings().value(key + "/build", QSettings().value(key)).toString()
                                  : ToolDiscovery::compiler(QString::fromStdString(plan.language.lspId),
                                                            QString::fromStdString(plan.build.program));
        if (!ToolDiscovery::projectSdk().root.isEmpty() && override.isEmpty()) {
            error =
                "Selected SDK does not contain the compiler: " + QString::fromStdString(plan.build.program);
            return false;
        }
        if (!override.isEmpty())
            stages[0].program = override.toStdString();
    }
    if (execute && plan.run.program != plan.outputPath) {
        const auto override = QSettings()
                                  .value("toolchains/" + QString::fromStdString(plan.language.lspId) + "/run")
                                  .toString();
        if (!ToolDiscovery::projectSdk().root.isEmpty()) {
            auto runtime = ToolDiscovery::project()
                               .value("tools")
                               .toObject()
                               .value(QString::fromStdString(plan.language.lspId))
                               .toObject()
                               .value("run")
                               .toString();
            if (runtime.isEmpty())
                runtime = ToolDiscovery::sdkExecutable(QString::fromStdString(plan.run.program));
            if (runtime.isEmpty()) {
                error =
                    "Selected SDK does not contain the runtime: " + QString::fromStdString(plan.run.program);
                return false;
            }
            stages.back().program = runtime.toStdString();
        } else if (!override.isEmpty())
            stages.back().program = override.toStdString();
    }
    return launch(std::move(stages), execute, std::move(output), std::move(finished), error);
}

bool CodeRunner::startProject(const QString& sourcePath, const QString& executable, bool execute,
                              std::function<void(const QString&)> output,
                              std::function<void(bool, int)> finished, QString& error) {
    if (active_) {
        error = "Another build/run is already active";
        return false;
    }
    const auto plan = ts::cmakeProjectPlanFor(sourcePath.toStdString(), "", "",
                                              execute ? executable.toStdString() : std::string{});
    if (!plan.supported) {
        error = QString::fromStdString(plan.explanation);
        return false;
    }
    if (execute && !plan.run.valid) {
        error = "Choose the executable relative to the CMake build directory";
        return false;
    }
    if (!QDir().mkpath(QString::fromStdString(plan.buildDirectory))) {
        error = "Cannot create CMake build directory";
        return false;
    }
    ts::Array<ts::ProcessSpec> stages;
    stages.pushBack(plan.configure);
    stages.pushBack(plan.build);
    if (execute)
        stages.pushBack(plan.run);
    return launch(std::move(stages), execute, std::move(output), std::move(finished), error);
}

void CodeRunner::complete(bool success, int code) {
    if (!active_)
        return;
    active_ = false;
    emit stateChanged(false, false);
    deadline_.stop();
    auto finished = std::move(finished_);
    if (finished)
        finished(success, code);
}

void CodeRunner::stop() {
    if (!active_)
        return;
    stopping_ = true;
    stopCode_ = 130;
    deadline_.stop();
    tree_.terminate();
    process_.kill();
}

void CodeRunner::sendInput(const QString& line) {
    if (active_ && !compiling_)
        process_.write((line + "\n").toUtf8());
}
