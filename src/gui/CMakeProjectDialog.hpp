#pragma once
#include "../application/CMakeFileApi.hpp"
#include "CodeRunner.hpp"
#include <QDialog>
#include <QProcess>
#include <functional>
class QLineEdit;
class QComboBox;
class QLabel;
class QPushButton;
class QCloseEvent;
enum class ProjectAction { Configure, Build, Run };

class CMakeProjectDialog final : public QDialog {
    QLineEdit *root_ = nullptr, *build_ = nullptr;
    QComboBox *generator_ = nullptr, *configuration_ = nullptr, *target_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton *configure_ = nullptr, *compile_ = nullptr, *run_ = nullptr, *stop_ = nullptr;
    QList<CMakeTarget> targets_;
    CodeRunner runner_;
    QProcess capabilities_;
    std::function<void(const QString&)> output_;
    bool closing_ = false;
    ProjectAction initialAction_ = ProjectAction::Configure;
    CMakeSettings settings() const;
    void setBusy(bool busy);
    void configureProject();
    void buildProject(bool execute);

  protected:
    void reject() override;

  public:
    CMakeProjectDialog(const QString& root, std::function<void(const QString&)> output,
                       QWidget* parent = nullptr, ProjectAction action = ProjectAction::Configure);
    ~CMakeProjectDialog() override;
};
