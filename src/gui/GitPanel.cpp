#include "GitPanel.hpp"
#include "UiLanguage.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>
#include <memory>

GitPanel::GitPanel(QWidget* parent, std::function<void(const QString&, int)> open)
    : QWidget(parent), open_(std::move(open)) {
    setObjectName("gitPanel");
    auto* layout = new QVBoxLayout(this);
    auto* row = new QHBoxLayout;
    heading_ = new QLabel("Git");
    heading_->setTextFormat(Qt::PlainText);
    row->addWidget(heading_, 1);
    auto* refresh = new QPushButton(UiLanguage::tr("Refresh"));
    refresh->setObjectName("gitRefresh");
    row->addWidget(refresh);
    layout->addLayout(row);
    state_ = new QLabel;
    state_->setTextFormat(Qt::PlainText);
    state_->setWordWrap(true);
    layout->addWidget(state_);
    auto* split = new QSplitter(Qt::Vertical);
    layout->addWidget(split, 1);
    tabs_ = new QTabWidget;
    split->addWidget(tabs_);
    changes_ = new QListWidget;
    changes_->setObjectName("gitChanges");
    history_ = new QListWidget;
    history_->setObjectName("gitHistory");
    blame_ = new QListWidget;
    blame_->setObjectName("gitBlame");
    tabs_->addTab(changes_, UiLanguage::tr("Changes"));
    tabs_->addTab(history_, UiLanguage::tr("History"));
    tabs_->addTab(blame_, UiLanguage::tr("Line authorship"));
    diff_ = new QPlainTextEdit;
    diff_->setObjectName("gitDiff");
    diff_->setReadOnly(true);
    diff_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    split->addWidget(diff_);
    split->setSizes({280, 320});
    connect(refresh, &QPushButton::clicked, this, &GitPanel::refresh);
    connect(changes_, &QListWidget::currentRowChanged, this, [this] { showChange(); });
    connect(changes_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        open_(QDir(root_).filePath(item->data(Qt::UserRole).toString()), 1);
    });
    connect(history_, &QListWidget::currentRowChanged, this, [this] { showCommit(); });
    connect(blame_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        open_(file_, item->data(Qt::UserRole).toInt());
        diff_->setPlainText(item->toolTip());
    });
    refreshTimer_.setInterval(5000);
    connect(&refreshTimer_, &QTimer::timeout, this, [this] {
        if (isVisible() && statusProcess_.state() == QProcess::NotRunning)
            this->refresh();
    });
    refreshTimer_.start();
    blameTimer_.setSingleShot(true);
    blameTimer_.setInterval(300);
    connect(&blameTimer_, &QTimer::timeout, this, &GitPanel::blameFile);
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 2)
            blameFile();
    });
}

GitPanel::~GitPanel() {
    for (auto* process : {&statusProcess_, &historyProcess_, &detailProcess_, &blameProcess_}) {
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
}

void GitPanel::run(QProcess& process, const QStringList& args,
                   const std::function<void(const QByteArray&, const QString&)>& result) {
    process.disconnect(this);
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(1000);
    }
    auto* timer = new QTimer(&process);
    timer->setSingleShot(true);
    timer->setInterval(10000);
    connect(timer, &QTimer::timeout, &process, [&process] { process.kill(); });
    auto output = std::make_shared<QByteArray>();
    auto errors = std::make_shared<QByteArray>();
    auto limited = std::make_shared<bool>(false);
    connect(&process, &QProcess::readyReadStandardOutput, this, [&process, output, limited] {
        *output += process.readAllStandardOutput();
        if (output->size() > 4 * 1024 * 1024) {
            *limited = true;
            process.kill();
        }
    });
    connect(&process, &QProcess::readyReadStandardError, this, [&process, errors] {
        *errors += process.readAllStandardError();
        if (errors->size() > 65536)
            errors->truncate(65536);
    });
    connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [result, output, errors, limited, timer](int code, QProcess::ExitStatus status) {
                timer->stop();
                timer->deleteLater();
                result(*output, *limited ? QString(UiLanguage::tr("Git output limit exceeded (4 MiB)"))
                                : code == 0 && status == QProcess::NormalExit ? QString{}
                                : QString::fromUtf8(*errors).trimmed().isEmpty()
                                    ? QString(UiLanguage::tr("Git failed or timed out"))
                                    : QString::fromUtf8(*errors));
            });
    connect(&process, &QProcess::errorOccurred, this,
            [result, timer, &process](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) {
                    timer->stop();
                    timer->deleteLater();
                    result({}, UiLanguage::tr("Git not found: ") + process.errorString());
                }
            });
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("GIT_OPTIONAL_LOCKS", "0");
    environment.insert("GIT_TERMINAL_PROMPT", "0");
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(root_.isEmpty() ? project_ : root_);
    process.start("git", QStringList{"--no-pager", "--no-optional-locks", "-c", "core.quotepath=false", "-c",
                                     "core.fsmonitor=false"} +
                             args);
    timer->start();
}

void GitPanel::setProject(const QString& path) {
    const auto canonical = QDir(path).canonicalPath();
    if (project_ == canonical)
        return;
    for (auto* process : {&statusProcess_, &historyProcess_, &detailProcess_, &blameProcess_}) {
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning)
            process->kill();
    }
    project_ = canonical;
    root_.clear();
    if (availability_)
        availability_(false);
    changes_->clear();
    history_->clear();
    blame_->clear();
    diff_->clear();
    refresh();
}

void GitPanel::refresh() {
    if (project_.isEmpty() || !QDir(project_).exists()) {
        state_->setText(UiLanguage::tr("Open a project folder"));
        return;
    }
    run(statusProcess_, {"rev-parse", "--show-toplevel"},
        [this](const QByteArray& out, const QString& error) {
            if (!error.isEmpty()) {
                root_.clear();
                if (availability_)
                    availability_(false);
                heading_->setText(UiLanguage::tr("Git · no repository"));
                state_->setText(error);
                changes_->clear();
                history_->clear();
                return;
            }
            root_ = QString::fromUtf8(out).trimmed();
            if (availability_)
                availability_(true);
            run(statusProcess_, {"status", "--porcelain=v1", "-z", "--branch", "--untracked-files=normal"},
                [this](const QByteArray& data, const QString& issue) {
                    if (!issue.isEmpty()) {
                        state_->setText(issue);
                        return;
                    }
                    const auto selected = changes_->currentItem()
                                              ? changes_->currentItem()->data(Qt::UserRole).toString()
                                              : QString{};
                    QSignalBlocker blocker(changes_);
                    changes_->clear();
                    const auto entries = data.split('\0');
                    for (int i = 0; i < entries.size(); ++i) {
                        const auto entry = entries[i];
                        if (entry.startsWith("## ")) {
                            heading_->setText("Git · " + QString::fromUtf8(entry.mid(3)));
                            continue;
                        }
                        if (entry.size() < 4)
                            continue;
                        const auto status = QString::fromUtf8(entry.left(2));
                        const auto path = QString::fromUtf8(entry.mid(3));
                        auto* item = new QListWidgetItem(status + "  " + path, changes_);
                        item->setData(Qt::UserRole, path);
                        item->setData(Qt::UserRole + 1, status);
                        item->setToolTip(UiLanguage::tr("Click: diff · double-click: open file"));
                        if (path == selected)
                            changes_->setCurrentItem(item);
                        if (status.contains('R') || status.contains('C'))
                            ++i;
                    }
                    state_->setText(QString(UiLanguage::tr("%1 changed files · local view of files on disk"))
                                        .arg(changes_->count()));
                });
            run(historyProcess_, {"log", "-60", "--date=short", "--format=%H%x1f%h%x1f%an%x1f%ad%x1f%s"},
                [this](const QByteArray& data, const QString& issue) {
                    if (!issue.isEmpty()) {
                        history_->clear();
                        return;
                    }
                    QSignalBlocker blocker(history_);
                    const auto selected = history_->currentItem()
                                              ? history_->currentItem()->data(Qt::UserRole).toString()
                                              : QString{};
                    history_->clear();
                    for (const auto& row : QString::fromUtf8(data).split('\n', Qt::SkipEmptyParts)) {
                        const auto fields = row.split(QChar(31));
                        if (fields.size() < 5)
                            continue;
                        auto* item = new QListWidgetItem(
                            fields[1] + "  " + fields[4] + "\n" + fields[2] + " · " + fields[3], history_);
                        item->setData(Qt::UserRole, fields[0]);
                        if (fields[0] == selected)
                            history_->setCurrentItem(item);
                    }
                });
            if (tabs_->currentIndex() == 2)
                blameFile();
        });
}

void GitPanel::showChange() {
    auto* item = changes_->currentItem();
    if (!item)
        return;
    const auto path = item->data(Qt::UserRole).toString();
    if (item->data(Qt::UserRole + 1).toString() == "??") {
        QFile file(QDir(root_).filePath(path));
        diff_->setPlainText(file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.read(1024 * 1024))
                                                           : file.errorString());
        return;
    }
    run(detailProcess_, {"diff", "--no-ext-diff", "--no-textconv", "HEAD", "--", path},
        [this](const QByteArray& data, const QString& error) {
            diff_->setPlainText(error.isEmpty() ? QString::fromUtf8(data) : error);
        });
}

void GitPanel::showCommit() {
    auto* item = history_->currentItem();
    if (!item)
        return;
    const auto hash = item->data(Qt::UserRole).toString();
    run(detailProcess_,
        {"show", "--no-ext-diff", "--no-textconv", "--format=fuller", "--stat", "--patch", hash, "--"},
        [this](const QByteArray& data, const QString& error) {
            diff_->setPlainText(error.isEmpty() ? QString::fromUtf8(data) : error);
        });
}

void GitPanel::setFile(const QString& path, int line) {
    const auto canonical = QFileInfo(path).canonicalFilePath();
    const bool changed = file_ != canonical;
    file_ = canonical;
    line_ = line;
    if (changed && tabs_->currentIndex() == 2)
        blameTimer_.start();
    else if (tabs_->currentIndex() == 2 && line > 0 && line <= blame_->count()) {
        blame_->setCurrentRow(line - 1);
        blame_->scrollToItem(blame_->currentItem());
    }
}

void GitPanel::blameFile() {
    blame_->clear();
    if (root_.isEmpty() || file_.isEmpty())
        return;
    const auto relative = QDir(root_).relativeFilePath(file_);
    if (relative.startsWith("../"))
        return;
    run(blameProcess_, {"blame", "--line-porcelain", "--", relative},
        [this](const QByteArray& data, const QString& error) {
            if (!error.isEmpty()) {
                diff_->setPlainText(error);
                return;
            }
            QString author, summary, hash;
            int line = 0;
            for (const auto& row : QString::fromUtf8(data).split('\n')) {
                if (row.startsWith("author "))
                    author = row.mid(7);
                else if (row.startsWith("summary "))
                    summary = row.mid(8);
                else if (row.startsWith('\t')) {
                    auto* item = new QListWidgetItem(
                        QString::number(line) + "  " + author + " · " + row.mid(1), blame_);
                    item->setData(Qt::UserRole, line);
                    item->setToolTip(hash + "\n" + author + "\n" + summary);
                } else if (row.size() > 40 && row[40] == ' ') {
                    const auto fields = row.split(' ');
                    if (fields.size() >= 3) {
                        hash = fields[0];
                        line = fields[2].toInt();
                    }
                }
            }
            if (line_ > 0 && line_ <= blame_->count()) {
                blame_->setCurrentRow(line_ - 1);
                blame_->scrollToItem(blame_->currentItem());
            }
        });
}
