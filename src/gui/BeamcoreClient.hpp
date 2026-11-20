#pragma once

#include "../platform/ProcessTree.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

class BeamcoreClient final : public QObject {
    Q_OBJECT
    QProcess process_;
    ProcessTree tree_;
    QTemporaryDir state_;
    QTimer deadline_;
    QByteArray pending_;
    bool ready_ = false;
    bool stopping_ = false;
    int nextId_ = 0, activeId_ = -1;
    QJsonObject queued_;
    void send(const QJsonObject& message);
    void consume();
    void request(const QJsonObject& message);

  public:
    explicit BeamcoreClient(QObject* parent = nullptr);
    ~BeamcoreClient() override;
    static QString executable();
    bool start(QString& error);
    void requestProviders();
    void saveProvider(const QJsonObject& provider);
    void removeProvider(const QString& name);
    void chat(const QJsonArray& messages, const QString& provider, const QString& model);
    void inferenceResult(int id, const QString& content);
    void stop();

    bool running() const {
        return process_.state() != QProcess::NotRunning;
    }

  signals:
    void providersReceived(const QJsonArray& providers);
    void inferenceRequested(int id, const QJsonArray& messages, int maxTokens);
    void responseReceived(const QString& content);
    void failed(const QString& error);
};
