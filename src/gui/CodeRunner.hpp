#pragma once
#include "../platform/ProcessTree.hpp"
#include "ts/LanguageTools.hpp"
#include "ts/ProjectTools.hpp"
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringDecoder>
#include <QTimer>
#include <functional>

class CodeRunner final : public QObject {
    Q_OBJECT
    QProcess process_;
    ProcessTree tree_;
    QStringDecoder stdoutDecoder_{QStringDecoder::Utf8};
    QStringDecoder stderrDecoder_{QStringDecoder::Utf8};
    bool stopping_ = false;
    int stopCode_ = 130;
    QTimer deadline_;
    ts::Array<ts::ProcessSpec> stages_;
    std::size_t stageIndex_ = 0;
    bool active_ = false;
    bool sequenceRun_ = false;
    bool compiling_ = false;
    qint64 displayedBytes_ = 0;
    int buildTimeout_ = 180000, runTimeout_ = 30000;
    std::function<void(const QString&)> output_;
    std::function<void(bool, int)> finished_;
    void beginStage();
    void complete(bool success, int code);
    void drainOutput();
    bool launch(ts::Array<ts::ProcessSpec> steps, bool execute, std::function<void(const QString&)> output,
                std::function<void(bool, int)> finished, QString& error);

  public:
    explicit CodeRunner(QObject* parent = nullptr);
    ~CodeRunner() override;
    void setTimeouts(int buildMilliseconds, int runMilliseconds);
    bool start(const QString& sourcePath, bool execute, std::function<void(const QString&)> output,
               std::function<void(bool, int)> finished, QString& error);
    bool startProject(const QString& sourcePath, const QString& executable, bool execute,
                      std::function<void(const QString&)> output, std::function<void(bool, int)> finished,
                      QString& error);

    bool startCommands(ts::Array<ts::ProcessSpec> commands, bool execute,
                       std::function<void(const QString&)> output, std::function<void(bool, int)> finished,
                       QString& error) {
        return launch(std::move(commands), execute, std::move(output), std::move(finished), error);
    }

    bool busy() const noexcept {
        return active_;
    }

    void stop();
    void sendInput(const QString& line);
  signals:
    void stateChanged(bool active, bool program);
};
