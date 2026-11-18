#pragma once
#include "../platform/ProcessTree.hpp"
#include <QProcess>
#include <QStringDecoder>
#include <QWidget>
#include <functional>

class QLineEdit;
class QPlainTextEdit;

class TerminalWidget final : public QWidget {
    QProcess process_;
    ProcessTree tree_;
    QStringDecoder decoder_{QStringDecoder::Utf8};
    QLineEdit* input_;
    QPlainTextEdit* output_;
    std::function<QString()> directory_;
    QByteArray pending_;
    void append(const QString& text);
    void start();
    void submit();
    void stop();

  public:
    explicit TerminalWidget(std::function<QString()> directory, QWidget* parent = nullptr);
    ~TerminalWidget() override;
};
