#pragma once
#include "../platform/ProcessTree.hpp"
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>

struct DatabaseConnection {
    QString driver, database, host, user, password, options, pluginPath;
    int port = -1;
    int timeoutSeconds = 30;
};

class DatabaseSession final : public QObject {
    Q_OBJECT
    QProcess process_;
    ProcessTree tree_;
    QTimer deadline_;
    QByteArray pending_;
    QString executable_;
    QString operation_;
    bool busy_ = false, connected_ = false;
    int serial_ = 0;
    int timeout_ = 30000;
    void consume();
    void request(QJsonObject message);

  public:
    explicit DatabaseSession(QObject* parent = nullptr, QString executable = {});
    ~DatabaseSession() override;
    void connectTo(const DatabaseConnection& connection);
    void browse(const QString& table, int offset = 0, int limit = 200);
    void query(const QString& sql, int offset = 0, int limit = 200);
    void queryPage(int offset, int limit = 200);
    void apply(int page, const QJsonArray& changes);
    void cancel();

    bool busy() const {
        return busy_;
    }

    bool connected() const {
        return connected_;
    }

    bool running() const {
        return process_.state() != QProcess::NotRunning;
    }

    QString operation() const {
        return operation_;
    }

  signals:
    void busyChanged(bool busy);
    void connectedChanged(bool connected);
    void resultReady(const QJsonObject& result);
    void failed(const QString& message);
};

int runDatabaseWorker(int argc, char** argv);
