#pragma once
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>

class BackgroundJob final : public QObject {
    Q_OBJECT
  public:
    using Cancel = std::function<bool()>;
    using Progress = std::function<void(int, int)>;
    using Work = std::function<void(const Cancel&, const Progress&)>;
    using Finished = std::function<void(const QString&, bool)>;

  private:
    struct State {
        std::atomic_bool cancelled{false};
        QString error;
    };

    QFutureWatcher<void> watcher_;
    std::shared_ptr<State> state_;
    Finished finished_;
    Progress progress_;
    bool active_ = false;
    unsigned long long generation_ = 0;

  public:
    explicit BackgroundJob(QObject* parent = nullptr);
    ~BackgroundJob() override;

    bool busy() const {
        return active_;
    }

    bool start(Work work, Finished done, Progress progress = {});
    void cancel();
};
