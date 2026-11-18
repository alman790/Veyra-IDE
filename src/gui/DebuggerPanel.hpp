#pragma once
#include "../platform/ProcessTree.hpp"
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QWidget>
#include <functional>
class QPlainTextEdit;
class QTreeWidget;
class QListWidget;
class QLabel;

class DebuggerPanel final : public QWidget {
    QProcess process_;
    ProcessTree tree_;
    QTemporaryDir scripts_;
    QPlainTextEdit* output_;
    QTreeWidget* variables_;
    QListWidget* stack_;
    QLabel* status_;
    QByteArray pending_;
    QJsonObject launch_;
    bool ready_ = false;
    unsigned long long generation_ = 0;
    std::function<void(const QString&, int)> location_;
    void consume();
    void acceptMessage(const QJsonObject& message);

  public:
    explicit DebuggerPanel(QWidget* parent, std::function<void(const QString&, int)> location);
    ~DebuggerPanel() override;
    bool start(const QString& executable, const QString& directory, const QJsonObject& breakpoints,
               QString& error);
    void send(QJsonObject command);
    void setBreakpoints(const QJsonObject& points);

    bool busy() const {
        return process_.state() != QProcess::NotRunning;
    }

    void stop();
};
