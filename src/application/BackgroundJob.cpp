#include "BackgroundJob.hpp"
#include <QMetaObject>
#include <QtConcurrent/QtConcurrentRun>
#include <exception>

BackgroundJob::BackgroundJob(QObject* parent) : QObject(parent) {
    connect(&watcher_, &QFutureWatcher<void>::finished, this, [this] {
        active_ = false;
        auto finished = std::move(finished_);
        const auto state = state_;
        progress_ = {};
        if (finished)
            finished(state->error, state->cancelled.load());
    });
}

BackgroundJob::~BackgroundJob() {
    finished_ = {};
    progress_ = {};
    cancel();
    watcher_.waitForFinished();
}

void BackgroundJob::cancel() {
    if (state_)
        state_->cancelled.store(true);
}

bool BackgroundJob::start(Work work, Finished done, Progress progress) {
    if (active_)
        return false;
    active_ = true;
    state_ = std::make_shared<State>();
    finished_ = std::move(done);
    progress_ = std::move(progress);
    const auto state = state_;
    const auto generation = ++generation_;
    watcher_.setFuture(QtConcurrent::run([this, state, generation, work = std::move(work)] {
        const Cancel cancelled = [state] { return state->cancelled.load(); };
        const Progress update = [this, generation](int current, int total) {
            QMetaObject::invokeMethod(
                this,
                [this, generation, current, total] {
                    if (active_ && generation_ == generation && progress_)
                        progress_(current, total);
                },
                Qt::QueuedConnection);
        };
        try {
            work(cancelled, update);
        } catch (const std::exception& e) {
            state->error = QString::fromUtf8(e.what());
        } catch (...) {
            state->error = "Unknown background operation error.";
        }
    }));
    return true;
}
