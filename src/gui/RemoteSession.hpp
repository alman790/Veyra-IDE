#pragma once
#include "../platform/ProcessTree.hpp"
#include <QJsonArray>
#include <QObject>
#include <QProcess>
#include <QTimer>

struct RemoteConnection {
    QString host, user, root, identity, knownHosts, executable;
    int port = 0;
    QString configurationFile;
};

class RemoteSession final : public QObject {
    Q_OBJECT
    QProcess process_;
    ProcessTree tree_;
    QTimer deadline_;
    RemoteConnection connection_;
    QString operation_, path_;
    QByteArray output_, errors_, input_;
    bool closeInput_ = true, busy_ = false, cancelled_ = false;
    QString prefix(const QString& relative, bool file) const;
    void start(const QString& operation, const QString& path, const QString& script,
               const QByteArray& input = {}, bool closeInput = true, int timeout = 30000);

  public:
    explicit RemoteSession(QObject* parent = nullptr);
    ~RemoteSession() override;
    static QString quote(const QString& text);
    static bool validPath(const QString& relative, bool allowEmpty = false);
    static QString knownHostsPath();
    static QStringList arguments(const RemoteConnection& connection);
    static QString sshProgram(const RemoteConnection& connection);
    void configure(const RemoteConnection& connection);
    void connectTo();
    void list(const QString& relative = {});
    void read(const QString& relative);
    void write(const QString& relative, const QByteArray& text, const QString& fingerprint);
    void run(const QString& command);
    void discover(const QStringList& programs);
    void sendInput(const QString& line);
    void finishInput();
    void cancel();

    bool busy() const {
        return busy_;
    }

    QString activePath() const {
        return path_;
    }

    const RemoteConnection& connection() const {
        return connection_;
    }

  signals:
    void busyChanged(bool busy);
    void connected(const QString& root);
    void directoryReady(const QString& path, const QJsonArray& entries);
    void fileReady(const QString& path, const QByteArray& contents, const QString& fingerprint);
    void saved(const QString& path, const QString& fingerprint);
    void toolsReady(const QJsonArray& tools);
    void commandOutput(const QByteArray& bytes);
    void commandFinished(int code);
    void failed(const QString& message);
};
