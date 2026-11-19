#include "BeamcoreClient.hpp"
#include "ToolDiscovery.hpp"
#include "UiLanguage.hpp"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

BeamcoreClient::BeamcoreClient(QObject* parent) : QObject(parent) {
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        stop();
        emit failed(UiLanguage::tr("The agent request timed out."));
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, &BeamcoreClient::consume);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] { process_.readAllStandardError(); });
    tree_.configure(process_);
    connect(&process_, &QProcess::started, this, [this] {
        QString error;
        if (!tree_.attach(process_.processId(), error)) {
            process_.kill();
            emit failed(error);
        }
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            deadline_.stop();
            emit failed(
                UiLanguage::tr("Beamcore could not be started. Check its executable in the agent settings."));
        }
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) {
                tree_.reset();
                deadline_.stop();
                ready_ = false;
                stopping_ = false;
                if (activeId_ >= 0 || !queued_.isEmpty()) {
                    activeId_ = -1;
                    queued_ = {};
                    emit failed(UiLanguage::tr("Beamcore stopped before completing the request."));
                }
            });
}

BeamcoreClient::~BeamcoreClient() {
    process_.disconnect(this);
    stop();
    if (process_.state() != QProcess::NotRunning)
        process_.waitForFinished(1000);
}

QString BeamcoreClient::executable() {
    auto configured = QSettings().value("agent/beamcoreExecutable").toString();
    if (configured.startsWith("~/"))
        configured = QDir::homePath() + configured.mid(1);
    return ToolDiscovery::find(configured.isEmpty() ? "beamcore" : configured);
}

bool BeamcoreClient::start(QString& error) {
    if (running()) {
        if (stopping_) {
            error = UiLanguage::tr("The previous agent process is stopping. Try again shortly.");
            return false;
        }
        return true;
    }
    stopping_ = false;
    const auto program = executable();
    if (program.isEmpty() || !state_.isValid()) {
        error = UiLanguage::tr("Install Beamcore or choose its executable to use its provider runtime.");
        return false;
    }
    QFile source(":/agent/bridge.exs");
    QFile bridge(state_.filePath("bridge.exs"));
    if (!source.open(QIODevice::ReadOnly) || !bridge.open(QIODevice::WriteOnly) ||
        bridge.write(source.readAll()) < 0) {
        error = UiLanguage::tr("The Beamcore adapter could not be prepared.");
        return false;
    }
    bridge.close();
    const auto copiedConfig = state_.filePath("config.dets");
    if (QFile::exists(copiedConfig) && !QFile::remove(copiedConfig)) {
        error = UiLanguage::tr("The isolated Beamcore configuration could not be refreshed.");
        return false;
    }
    auto environment = ToolDiscovery::environment();
    environment.insert("VEYRA_AGENT_STATE", state_.path());
    const auto config = QSettings().value("agent/beamcoreConfig").toString();
    if (!config.isEmpty())
        environment.insert("VEYRA_BEAMCORE_CONFIG", config);
    environment.remove("TELEGRAM_BOT_TOKEN");
    environment.remove("DISCORD_BOT_TOKEN");
    environment.remove("BEAMCORE_TARGET_NODE");
    process_.setProcessEnvironment(environment);
    process_.setWorkingDirectory(state_.path());
    ready_ = false;
    pending_.clear();
    process_.start(program,
                   {"eval", "Code.eval_file(System.fetch_env!(\"VEYRA_AGENT_STATE\") <> \"/bridge.exs\")"});
    deadline_.start(20000);
    return true;
}

void BeamcoreClient::send(const QJsonObject& message) {
    process_.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void BeamcoreClient::requestProviders() {
    QString error;
    if (!start(error)) {
        emit failed(error);
        return;
    }
    queued_ = {{"method", "providers"}};
    if (ready_)
        send(queued_);
}

void BeamcoreClient::chat(const QJsonArray& messages, const QString& provider, const QString& model) {
    QString error;
    if (!start(error)) {
        emit failed(error);
        return;
    }
    activeId_ = ++nextId_;
    queued_ = {
        {"method", "chat"},     {"id", activeId_}, {"backend", provider.isEmpty() ? "local" : "remote"},
        {"provider", provider}, {"model", model},  {"messages", messages},
        {"max_tokens", 768}};
    if (ready_) {
        deadline_.start(180000);
        send(queued_);
    }
}

void BeamcoreClient::inferenceResult(int id, const QString& content) {
    if (id == activeId_ && running())
        send({{"method", "inference_result"}, {"id", id}, {"content", content}});
}

void BeamcoreClient::consume() {
    pending_ += process_.readAllStandardOutput();
    if (pending_.size() > 2 * 1024 * 1024) {
        stop();
        emit failed(UiLanguage::tr("The agent response exceeded its size limit."));
        return;
    }
    int newline = -1;
    while ((newline = pending_.indexOf('\n')) >= 0) {
        const auto line = pending_.left(newline);
        pending_.remove(0, newline + 1);
        if (!line.startsWith("VEYRA_AGENT "))
            continue;
        const auto message = QJsonDocument::fromJson(line.mid(12)).object();
        const auto type = message.value("type").toString();
        if (type == "ready") {
            ready_ = true;
            deadline_.start(180000);
            if (!queued_.isEmpty())
                send(queued_);
        } else if (type == "providers") {
            queued_ = {};
            deadline_.stop();
            emit providersReceived(message.value("providers").toArray());
        } else if (type == "inference" && activeId_ >= 0 && message.value("id").toInt(-1) == activeId_)
            emit inferenceRequested(activeId_, message.value("messages").toArray(),
                                    message.value("max_tokens").toInt(768));
        else if (type == "response" && activeId_ >= 0 && message.value("id").toInt(-1) == activeId_) {
            activeId_ = -1;
            queued_ = {};
            deadline_.stop();
            emit responseReceived(message.value("content").toString());
        } else if (type == "error") {
            activeId_ = -1;
            queued_ = {};
            deadline_.stop();
            emit failed(message.value("message").toString());
        }
    }
}

void BeamcoreClient::stop() {
    activeId_ = -1;
    queued_ = {};
    deadline_.stop();
    if (running()) {
        stopping_ = true;
        tree_.terminate();
        process_.kill();
    }
    ready_ = false;
    pending_.clear();
}
