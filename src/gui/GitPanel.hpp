#pragma once
#include <QProcess>
#include <QTimer>
#include <QWidget>
#include <functional>
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QTabWidget;

class GitPanel final : public QWidget {
    QString project_, root_, file_;
    int line_ = 1;
    QProcess statusProcess_, historyProcess_, detailProcess_, blameProcess_;
    QTimer refreshTimer_, blameTimer_;
    QLabel* heading_;
    QLabel* state_;
    QListWidget* changes_;
    QListWidget* history_;
    QListWidget* blame_;
    QPlainTextEdit* diff_;
    QTabWidget* tabs_;
    std::function<void(const QString&, int)> open_;
    std::function<void(bool)> availability_;
    void run(QProcess& process, const QStringList& args,
             const std::function<void(const QByteArray&, const QString&)>& result);
    void showChange();
    void showCommit();
    void blameFile();

  public:
    explicit GitPanel(QWidget* parent, std::function<void(const QString&, int)> open);
    ~GitPanel() override;
    void setProject(const QString& path);
    void setFile(const QString& path, int line);
    void refresh();

    bool hasRepository() const {
        return !root_.isEmpty();
    }

    void setAvailabilityCallback(std::function<void(bool)> callback) {
        availability_ = std::move(callback);
        if (availability_)
            availability_(hasRepository());
    }
};
