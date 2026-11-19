#pragma once
#include "../platform/ProcessTree.hpp"
#include "JsonRpcFramer.hpp"
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QStringDecoder>
#include <QWidget>
#include <functional>
class QPlainTextEdit;
class QTreeWidget;
class QListWidget;
class QLabel;
class QSocketNotifier;

class DebuggerPanel final : public QWidget {
    QProcess process_, target_;
    ProcessTree tree_, targetTree_;
    QPlainTextEdit* output_;
    QTreeWidget* variables_;
    QListWidget* stack_;
    QLabel* status_;
    JsonRpcFramer framer_;
    QJsonObject launch_, points_;
    QJsonArray frames_;
    QHash<int, std::function<void(const QJsonObject&)>> requests_;
    int sequence_ = 0, terminalRequest_ = 0, selected_ = 0;
    qint64 thread_ = 0;
    bool ready_ = false, paused_ = false, stopping_ = false, entryPending_ = false;
    unsigned long long generation_ = 0, snapshot_ = 0;
    int terminal_ = -1, terminalAnchor_ = -1;
    QSocketNotifier *terminalReader_ = nullptr, *terminalWriter_ = nullptr;
    QByteArray input_;
    QStringDecoder terminalDecoder_{QStringDecoder::Utf8};
    QStringDecoder outputDecoder_{QStringDecoder::Utf8}, errorDecoder_{QStringDecoder::Utf8};
    std::function<void(const QString&, int)> location_;
    void consume();
    void acceptMessage(const QJsonObject& message);
    void request(const QString& command, const QJsonObject& arguments = {},
                 std::function<void(const QJsonObject&)> callback = {});
    void configureBreakpoints(const QJsonObject& points, bool initial);
    void loadStack();
    void loadFrame(int index);
    void launchTerminal(const QJsonObject& message);
    void terminalResponse(bool success);
    void readTerminal();
    void writeTerminal();
    void closeTerminal();

  public:
    explicit DebuggerPanel(QWidget* parent, std::function<void(const QString&, int)> location);
    ~DebuggerPanel() override;
    static QString adapterPath();
    bool start(const QString& executable, const QString& directory, const QJsonObject& breakpoints,
               QString& error);
    void send(QJsonObject command);
    void setBreakpoints(const QJsonObject& points);

    bool busy() const {
        return process_.state() != QProcess::NotRunning;
    }

    void stop();
};
