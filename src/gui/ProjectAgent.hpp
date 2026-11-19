#pragma once

#include "../application/AgentContext.hpp"
#include "BeamcoreClient.hpp"
#include <memory>

class ProjectAgent final : public QObject {
    Q_OBJECT
    BackgroundJob work_;
    BeamcoreClient beamcore_;
    std::shared_ptr<AgentContext> context_;
    QJsonArray messages_, history_;
    QString provider_, model_, question_, historyRoot_;
    quint64 generation_ = 0;
    int steps_ = 0;
    QSet<QString> lookups_;
    bool finalizing_ = false;
    bool nativeFallback_ = false;
    bool corrected_ = false;
    bool busy_ = false, cancelling_ = false;
    void setBusy(bool busy);
    void fail(const QString& error);
    void request();
    void infer(int id, const QJsonArray& messages, int maxTokens);
    void accept(const QString& response);
    bool correct(const QString& response, const QString& error);

  public:
    explicit ProjectAgent(QObject* parent = nullptr);
    ~ProjectAgent() override;
    void start(const AgentInput& input, const QString& question, const QString& provider = {},
               const QString& model = {});
    void cancel();
    void clearHistory();

    bool busy() const {
        return busy_;
    }

    bool usesLocalInference() const {
        return busy_ && provider_.isEmpty();
    }

    const std::shared_ptr<AgentContext>& context() const {
        return context_;
    }

    BeamcoreClient* beamcore() {
        return &beamcore_;
    }

    static QString instructions(const QString& language);
    static std::string grammar();

  signals:
    void busyChanged(bool busy);
    void statusChanged(const QString& status);
    void contextReady(const QStringList& paths);
    void answerReady(const QString& answer);
    void proposalReady(const AgentProposal& proposal, const QString& preview);
    void failed(const QString& error);
};
