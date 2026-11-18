#pragma once
#include <QDialog>
#include <functional>
class QLineEdit;
class QListWidget;
class QStackedWidget;

class SettingsDialog final : public QDialog {
    QLineEdit* search_;
    QListWidget* sections_;
    QStackedWidget* pages_;
    std::function<void()> applied_;

  public:
    explicit SettingsDialog(QWidget* parent, std::function<void()> applied);

  private:
    bool apply();
};
