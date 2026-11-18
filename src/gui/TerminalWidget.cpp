#include "TerminalWidget.hpp"
#include "ToolDiscovery.hpp"
#include "UiLanguage.hpp"
#include <QDir>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>

TerminalWidget::TerminalWidget(std::function<QString()> directory, QWidget* parent)
    : QWidget(parent), directory_(std::move(directory)) {
    setObjectName("terminalWidget");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    output_ = new QPlainTextEdit;
    output_->setObjectName("terminalOutput");
    output_->setReadOnly(true);
    output_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    output_->document()->setMaximumBlockCount(2000);
    layout->addWidget(output_, 1);
    auto* controls = new QHBoxLayout;
    input_ = new QLineEdit;
    input_->setObjectName("terminalInput");
    input_->setPlaceholderText(UiLanguage::tr("Shell command / stdin · Enter"));
    controls->addWidget(input_, 1);
    auto* eof = new QPushButton("EOF");
    eof->setObjectName("terminalEof");
    auto* stopButton = new QPushButton(UiLanguage::tr("Stop"));
    stopButton->setObjectName("terminalStop");
    controls->addWidget(eof);
    controls->addWidget(stopButton);
    layout->addLayout(controls);
    process_.setProcessChannelMode(QProcess::MergedChannels);
    tree_.configure(process_);
    connect(input_, &QLineEdit::returnPressed, this, &TerminalWidget::submit);
    connect(eof, &QPushButton::clicked, this, [this] { process_.closeWriteChannel(); });
    connect(stopButton, &QPushButton::clicked, this, &TerminalWidget::stop);
    connect(&process_, &QProcess::started, this, [this] {
        QString error;
        if (!tree_.attach(process_.processId(), error))
            append(error + "\n");
        if (!pending_.isEmpty()) {
            process_.write(pending_);
            pending_.clear();
        }
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this,
            [this] { append(decoder_(process_.readAllStandardOutput())); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        append(process_.errorString() + "\n");
        if (error == QProcess::FailedToStart)
            pending_.clear();
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
                append(decoder_(process_.readAllStandardOutput()));
                tree_.reset();
                pending_.clear();
                append(QString("\n[Shell exited: %1; Enter starts a new session]\n").arg(code));
            });
}

TerminalWidget::~TerminalWidget() {
    process_.disconnect(this);
    stop();
    if (process_.state() != QProcess::NotRunning)
        process_.waitForFinished(1500);
}

void TerminalWidget::append(const QString& text) {
    auto cursor = output_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    output_->setTextCursor(cursor);
    output_->ensureCursorVisible();
}

void TerminalWidget::start() {
    decoder_.resetState();
    auto directory = directory_ ? directory_() : QString();
    if (!QDir(directory).exists() || directory.isEmpty())
        directory = QDir::homePath();
    process_.setWorkingDirectory(directory);
    process_.setProcessEnvironment(ToolDiscovery::environment());
#ifdef Q_OS_WIN
    const auto shell = qEnvironmentVariable("COMSPEC", "cmd.exe");
    process_.start(shell, {"/D", "/Q"});
#else
    process_.start("/bin/sh", {});
#endif
    append("[Shell: " + directory + "]\n");
}

void TerminalWidget::submit() {
    const auto text = input_->text();
    input_->clear();
    append("> " + text + "\n");
#ifdef Q_OS_WIN
    const auto bytes = (text + "\r\n").toLocal8Bit();
#else
    const auto bytes = (text + "\n").toUtf8();
#endif
    if (process_.state() == QProcess::NotRunning) {
        pending_ = bytes;
        start();
    } else if (process_.state() == QProcess::Starting) {
        pending_ += bytes;
    } else {
        process_.write(bytes);
    }
}

void TerminalWidget::stop() {
    pending_.clear();
    tree_.terminate();
    if (process_.state() != QProcess::NotRunning)
        process_.kill();
}
