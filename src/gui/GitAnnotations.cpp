#include "GitAnnotations.hpp"
#include "UiLanguage.hpp"
#include <QFileInfo>
#include <QFutureWatcher>
#include <QProcessEnvironment>
#include <QtConcurrent>

GitAnnotations::GitAnnotations(QObject* parent) : QObject(parent) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(800);
    refresh_.setInterval(15000);
    connect(&debounce_, &QTimer::timeout, this, &GitAnnotations::request);
    connect(&refresh_, &QTimer::timeout, this, [this] {
        if (!debounce_.isActive() && !process_)
            request();
    });
}

GitAnnotations::~GitAnnotations() {
    for (auto* process : findChildren<QProcess*>()) {
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
}

QString GitAnnotations::error() const {
    return path_.isEmpty() ? UiLanguage::tr("Save this file to view Git annotations.") : error_;
}

void GitAnnotations::setDocument(QTextDocument* document, const QString& path) {
    if (document_ == document && path_ == path)
        return;
    disconnect(changed_);
    document_ = document;
    path_ = path;
    if (document)
        changed_ =
            connect(document, &QTextDocument::contentsChange, this, [this](int, int removed, int added) {
                if (removed || added)
                    invalidate();
            });
    invalidate();
}

void GitAnnotations::setEnabled(bool enabled) {
    if (enabled_ == enabled)
        return;
    enabled_ = enabled;
    if (enabled)
        refresh_.start();
    else
        refresh_.stop();
    invalidate();
}

void GitAnnotations::invalidate() {
    ++generation_;
    if (process_) {
        process_->disconnect(this);
        connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process_,
                &QObject::deleteLater);
        if (process_->state() == QProcess::NotRunning)
            process_->deleteLater();
        else {
            connect(process_, &QProcess::started, process_, &QProcess::kill);
            process_->kill();
        }
        process_.clear();
    }
    lines_.clear();
    error_.clear();
    debounce_.stop();
    if (enabled_ && document_ && !path_.isEmpty())
        debounce_.start();
    emit updated();
}

QVector<LineAttribution> GitAnnotations::parse(const QByteArray& output, int lineCount) {
    QVector<LineAttribution> lines(lineCount);
    QHash<QByteArray, LineAttribution> commits;
    QByteArray hash;
    int start = 0, count = 0;
    for (const auto& row : output.split('\n')) {
        const auto fields = row.split(' ');
        if (fields.size() == 4 && (fields[0].size() == 40 || fields[0].size() == 64)) {
            bool ok = false;
            const int finalLine = fields[2].toInt(&ok);
            const int length = fields[3].toInt();
            if (!ok || finalLine < 1 || length < 1 || finalLine > lineCount ||
                length > lineCount - finalLine + 1)
                continue;
            hash = fields[0];
            start = finalLine - 1;
            count = length;
            commits[hash].commit = QString::fromLatin1(hash);
        } else if (!hash.isEmpty()) {
            auto& commit = commits[hash];
            if (row.startsWith("author "))
                commit.author = QString::fromUtf8(row.mid(7));
            else if (row.startsWith("author-time "))
                commit.timestamp = row.mid(12).toLongLong();
            else if (row.startsWith("summary "))
                commit.summary = QString::fromUtf8(row.mid(8));
            else if (row.startsWith("filename ")) {
                for (int i = start; i < start + count; ++i)
                    lines[i] = commit;
                hash.clear();
            }
        }
    }
    return lines;
}

void GitAnnotations::request() {
    if (!enabled_ || !document_ || path_.isEmpty())
        return;
    const auto generation = ++generation_;
    const int revision = document_->revision(), count = document_->blockCount();
    auto* process = new QProcess(this);
    process_ = process;
    process->setWorkingDirectory(QFileInfo(path_).absolutePath());
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("GIT_TERMINAL_PROMPT", "0");
    environment.insert("GIT_OPTIONAL_LOCKS", "0");
    process->setProcessEnvironment(environment);
    auto* deadline = new QTimer(process);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, process, &QProcess::kill);
    connect(process, &QProcess::errorOccurred, this,
            [this, process, generation](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart && generation == generation_) {
                    error_ = UiLanguage::tr("Git is not available.");
                    process_.clear();
                    process->deleteLater();
                    emit updated();
                }
            });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, generation, revision, count](int code, QProcess::ExitStatus status) {
                const auto output = process->readAllStandardOutput();
                const auto error = process->readAllStandardError();
                process->deleteLater();
                if (generation != generation_ || !document_ || document_->revision() != revision)
                    return;
                process_.clear();
                if (code != 0 || status != QProcess::NormalExit) {
                    lines_.clear();
                    error_ = QString::fromUtf8(error).trimmed();
                    if (error_.isEmpty())
                        error_ = UiLanguage::tr("Git annotations are unavailable.");
                    emit updated();
                    return;
                }
                auto* watcher = new QFutureWatcher<QVector<LineAttribution>>(this);
                connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, revision] {
                    if (generation == generation_ && document_ && document_->revision() == revision) {
                        lines_ = watcher->result();
                        error_.clear();
                        emit updated();
                    }
                    watcher->deleteLater();
                });
                watcher->setFuture(QtConcurrent::run([output, count] { return parse(output, count); }));
            });
    process->start("git", {"--no-pager", "blame", "--incremental", "--contents", "-", "--",
                           QFileInfo(path_).fileName()});
    process->write(document_->toPlainText().toUtf8());
    process->closeWriteChannel();
    deadline->start(30000);
}
