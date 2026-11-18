#pragma once
#include <QProcess>
#include <QString>

class ProcessTree final {
    qint64 pid_ = 0;
    void* job_ = nullptr;

  public:
    ProcessTree() = default;
    ProcessTree(const ProcessTree&) = delete;
    ProcessTree& operator=(const ProcessTree&) = delete;
    ~ProcessTree();
    void configure(QProcess& process);
    bool attach(qint64 pid, QString& error);
    void terminate();
    void reset();
};
