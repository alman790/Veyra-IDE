#pragma once
#include "../platform/ProcessTree.hpp"
#include "ToolDiscovery.hpp"
#include <QDialog>
#include <QJsonArray>
#include <QProcess>
class QLabel;
class QPushButton;
class QPlainTextEdit;

class SdkInstaller final : public QDialog {
    ToolDiscovery::DetectedSdk chosen_;
    QString root_, managed_, server_;
    QJsonObject recipe_;
    QJsonArray steps_;
    QProcess process_;
    ProcessTree tree_;
    int step_ = 0;
    QPushButton* install_;
    QLabel* state_;
    QPlainTextEdit* output_;
    QString expand(QString text) const;
    void next();

  protected:
    void reject() override;

  public:
    SdkInstaller(const ToolDiscovery::DetectedSdk& chosen, const QString& project, QWidget* parent);
    ~SdkInstaller() override;
};
