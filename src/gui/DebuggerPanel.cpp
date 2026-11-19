#include "DebuggerPanel.hpp"
#include "ToolDiscovery.hpp"
#include "UiLanguage.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSocketNotifier>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <memory>
#ifndef Q_OS_WIN
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

DebuggerPanel::DebuggerPanel(QWidget* parent, std::function<void(const QString&, int)> location)
    : QWidget(parent), location_(std::move(location)) {
    tree_.configure(process_);
    targetTree_.configure(target_);

    connect(&process_, &QProcess::started, this, [this] {
        QString error;
        tree_.attach(process_.processId(), error);
        request("initialize",
                {{"clientID", "veyra"},
                 {"adapterID", "lldb"},
                 {"pathFormat", "path"},
                 {"linesStartAt1", true},
                 {"columnsStartAt1", true},
                 {"supportsRunInTerminalRequest", true}},
                [this](const QJsonObject&) {
                    ready_ = true;
                    request("launch", launch_);
                });
    });
    auto* layout = new QVBoxLayout(this);
    auto* controls = new QHBoxLayout;
    for (const auto& entry : QList<QPair<QString, QString>>{{"Continue", "continue"},
                                                            {"Pause", "pause"},
                                                            {"Step over", "next"},
                                                            {"Step into", "stepIn"},
                                                            {"Step out", "stepOut"},
                                                            {"Stop", "stop"}}) {
        auto* button = new QPushButton(UiLanguage::text(entry.first));
        controls->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, command = entry.second] {
            if (command == "stop")
                stop();
            else
                send({{"command", command}});
        });
    }
    status_ = new QLabel(UiLanguage::tr("Debugger idle"));
    controls->addWidget(status_, 1);
    layout->addLayout(controls);
    auto* split = new QSplitter;
    stack_ = new QListWidget;
    stack_->setObjectName("debugCallStack");
    variables_ = new QTreeWidget;
    variables_->setObjectName("debugVariables");
    variables_->setHeaderLabels({UiLanguage::tr("Name"), UiLanguage::tr("Value"), UiLanguage::tr("Type")});
    output_ = new QPlainTextEdit;
    output_->setObjectName("debugConsole");
    output_->setReadOnly(true);
    output_->document()->setMaximumBlockCount(1000);
    split->addWidget(stack_);
    split->addWidget(variables_);
    split->addWidget(output_);
    split->setSizes({240, 360, 500});
    layout->addWidget(split, 1);
    auto* expression = new QLineEdit;
    expression->setObjectName("debugExpression");
    expression->setPlaceholderText(UiLanguage::tr("Evaluate expression in the selected stack frame · Enter"));
    layout->addWidget(expression);
    connect(expression, &QLineEdit::returnPressed, this, [this, expression] {
        send({{"command", "evaluate"},
              {"expression", expression->text()},
              {"frame", std::max(0, stack_->currentRow())}});
    });
    auto* input = new QLineEdit;
    input->setPlaceholderText(UiLanguage::tr("Program input · Enter"));
    layout->addWidget(input);
    connect(input, &QLineEdit::returnPressed, this, [this, input] {
        send({{"command", "stdin"}, {"text", input->text()}});
        input->clear();
    });
    connect(stack_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && !stack_->signalsBlocked())
            send({{"command", "frame"}, {"index", row}});
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, &DebuggerPanel::consume);
    connect(&process_, &QProcess::readyReadStandardError, this,
            [this] { output_->appendPlainText(QString::fromUtf8(process_.readAllStandardError())); });
    connect(&process_, &QProcess::errorOccurred, this, [this] {
        status_->setText(process_.errorString());
        output_->appendPlainText(process_.errorString());
    });
    connect(&process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
                readTerminal();
                closeTerminal();
                consume();
                requests_.clear();
                tree_.reset();
                ready_ = false;
                status_->setText(UiLanguage::tr("Debug session ended"));
                if (code != 0)
                    output_->appendPlainText(status_->text() + ": " + QString::number(code));
            });
    connect(&target_, &QProcess::started, this, [this] {
        QString error;
        targetTree_.attach(target_.processId(), error);
        terminalResponse(true);
    });
    connect(&target_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            terminalResponse(false);
    });
    connect(&target_, &QProcess::readyReadStandardOutput, this, [this] {
        output_->moveCursor(QTextCursor::End);
        output_->insertPlainText(outputDecoder_(target_.readAllStandardOutput()));
    });
    connect(&target_, &QProcess::readyReadStandardError, this, [this] {
        output_->moveCursor(QTextCursor::End);
        output_->insertPlainText(errorDecoder_(target_.readAllStandardError()));
    });
}

DebuggerPanel::~DebuggerPanel() {
    closeTerminal();
    process_.disconnect(this);
    target_.disconnect(this);
    targetTree_.terminate();
    if (target_.state() != QProcess::NotRunning) {
        target_.kill();
        target_.waitForFinished(1000);
    }
    tree_.terminate();
    if (busy()) {
        process_.terminate();
        if (!process_.waitForFinished(1500)) {
            process_.kill();
            process_.waitForFinished(500);
        }
    }
}

QString DebuggerPanel::adapterPath() {
    const auto configured = QSettings().value("debugger/adapter").toString();
    if (!configured.isEmpty())
        return ToolDiscovery::find(configured);
    for (const auto& name : {"lldb-dap", "lldb-vscode", "lldb-dap-22", "lldb-dap-21", "lldb-dap-20",
                             "lldb-dap-19", "lldb-vscode-18"}) {
        const auto path = ToolDiscovery::find(name);
        if (!path.isEmpty())
            return path;
    }
#ifdef Q_OS_MACOS
    QProcess xcrun;
    xcrun.start("/usr/bin/xcrun", {"--find", "lldb-dap"});
    if (xcrun.waitForFinished(3000) && xcrun.exitCode() == 0)
        return QString::fromUtf8(xcrun.readAllStandardOutput()).trimmed();
#endif
    return {};
}

bool DebuggerPanel::start(const QString& executable, const QString& directory, const QJsonObject& breakpoints,
                          QString& error) {
    if (busy() || target_.state() != QProcess::NotRunning) {
        error = "A debug session is already active.";
        return false;
    }
    if (!QFileInfo::exists(executable)) {
        error = "The debug executable was not found.";
        return false;
    }
    const auto adapter = adapterPath();
    if (adapter.isEmpty()) {
        error = "Select an LLDB DAP executable in Settings before debugging.";
        return false;
    }
    closeTerminal();
#ifndef Q_OS_WIN
    terminal_ = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (terminal_ < 0 || grantpt(terminal_) != 0 || unlockpt(terminal_) != 0) {
        error = QString::fromLocal8Bit(std::strerror(errno));
        closeTerminal();
        return false;
    }
    const auto slave = QString::fromLocal8Bit(ptsname(terminal_));
    terminalAnchor_ = ::open(slave.toLocal8Bit().constData(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (terminalAnchor_ < 0) {
        error = QString::fromLocal8Bit(std::strerror(errno));
        closeTerminal();
        return false;
    }
    termios terminalSettings{};
    if (tcgetattr(terminalAnchor_, &terminalSettings) != 0) {
        error = QString::fromLocal8Bit(std::strerror(errno));
        closeTerminal();
        return false;
    }
    cfmakeraw(&terminalSettings);
    if (tcsetattr(terminalAnchor_, TCSANOW, &terminalSettings) != 0) {
        error = QString::fromLocal8Bit(std::strerror(errno));
        closeTerminal();
        return false;
    }
    terminalReader_ = new QSocketNotifier(terminal_, QSocketNotifier::Read, this);
    terminalWriter_ = new QSocketNotifier(terminal_, QSocketNotifier::Write, this);
    terminalWriter_->setEnabled(false);
    connect(terminalReader_, &QSocketNotifier::activated, this, [this] { readTerminal(); });
    connect(terminalWriter_, &QSocketNotifier::activated, this, [this] { writeTerminal(); });
#endif
    ++generation_;
    ++snapshot_;
    sequence_ = 0;
    outputDecoder_ = QStringDecoder(QStringDecoder::Utf8);
    errorDecoder_ = QStringDecoder(QStringDecoder::Utf8);
    terminalRequest_ = 0;
    thread_ = 0;
    ready_ = paused_ = stopping_ = false;
    entryPending_ = true;
    framer_.clear();
    requests_.clear();
    frames_ = {};
    points_ = {};
    launch_ = {{"program", executable}, {"cwd", directory}, {"stopOnEntry", false}, {"runInTerminal", true}};
#ifndef Q_OS_WIN
    launch_["runInTerminal"] = false;
    launch_["disableASLR"] = false;
    launch_["launchCommands"] = QJsonArray{"process launch --stop-at-entry --stdin " + slave + " --stdout " +
                                           slave + " --stderr " + slave};
#endif
    launch_["breakpoints"] = breakpoints;
    output_->clear();
    stack_->clear();
    variables_->clear();
    process_.setProcessEnvironment(ToolDiscovery::environment());
    process_.setWorkingDirectory(directory);
    process_.start(adapter, {});
    status_->setText(UiLanguage::tr("Starting debugger…"));
    return true;
}

void DebuggerPanel::request(const QString& command, const QJsonObject& arguments,
                            std::function<void(const QJsonObject&)> callback) {
    if (!busy())
        return;
    const int id = ++sequence_;
    if (callback)
        requests_.insert(id, std::move(callback));
    process_.write(JsonRpcFramer::frame(
        {{"seq", id}, {"type", "request"}, {"command", command}, {"arguments", arguments}}));
}

void DebuggerPanel::consume() {
    QString error;
    if (!framer_.feed(
            process_.readAllStandardOutput(), [this](const QJsonObject& message) { acceptMessage(message); },
            error)) {
        output_->appendPlainText(error);
        stop();
    }
}

void DebuggerPanel::terminalResponse(bool success) {
    if (!terminalRequest_)
        return;
    QJsonObject reply{{"seq", ++sequence_},
                      {"type", "response"},
                      {"request_seq", terminalRequest_},
                      {"command", "runInTerminal"},
                      {"success", success}};
    if (success)
        reply["body"] =
            QJsonObject{{"processId", target_.processId()}, {"shellProcessId", target_.processId()}};
    else
        reply["message"] = target_.errorString();
    process_.write(JsonRpcFramer::frame(reply));
    terminalRequest_ = 0;
}

void DebuggerPanel::launchTerminal(const QJsonObject& message) {
    terminalRequest_ = message.value("seq").toInt();
    const auto arguments = message.value("arguments").toObject();
    QStringList args;
    for (const auto& arg : arguments.value("args").toArray())
        args << arg.toString();
    if (args.isEmpty() || target_.state() != QProcess::NotRunning) {
        terminalResponse(false);
        return;
    }
    auto environment = ToolDiscovery::environment();
    const auto supplied = arguments.value("env").toObject();
    for (auto it = supplied.begin(); it != supplied.end(); ++it) {
        if (it.value().isNull())
            environment.remove(it.key());
        else
            environment.insert(it.key(), it.value().toString());
    }
    target_.setProcessEnvironment(environment);
    target_.setWorkingDirectory(arguments.value("cwd").toString());
    const auto executable = args.takeFirst();
    target_.start(executable, args);
}

void DebuggerPanel::acceptMessage(const QJsonObject& message) {
    const auto type = message.value("type").toString();
    if (type == "request") {
        if (message.value("command") == "runInTerminal")
            launchTerminal(message);
        else
            process_.write(JsonRpcFramer::frame({{"seq", ++sequence_},
                                                 {"type", "response"},
                                                 {"request_seq", message.value("seq")},
                                                 {"command", message.value("command")},
                                                 {"success", false},
                                                 {"message", "Unsupported adapter request"}}));
        return;
    }
    if (type == "response") {
        auto callback = requests_.take(message.value("request_seq").toInt());
        if (!message.value("success").toBool()) {
            const auto error = message.value("message").toString(
                message.value("body").toObject().value("error").toObject().value("format").toString());
            output_->appendPlainText(error);
            status_->setText(UiLanguage::text(error));
            const auto command = message.value("command").toString();
            if (command == "initialize" || command == "launch") {
                stop();
                return;
            }
        }
        if (callback)
            callback(message.value("body").toObject());
        return;
    }
    if (type != "event")
        return;
    const auto event = message.value("event").toString();
    const auto body = message.value("body").toObject();
    if (event == "initialized" && !stopping_) {
        configureBreakpoints(launch_.take("breakpoints").toObject(), true);
    } else if (event == "stopped") {
        if (entryPending_) {
            entryPending_ = false;
            request("setFunctionBreakpoints", {{"breakpoints", QJsonArray{}}});
        }
        paused_ = true;
        thread_ = body.value("threadId").toInteger();
        status_->setText(UiLanguage::tr("Paused"));
        loadStack();
    } else if (event == "continued") {
        paused_ = false;
        ++snapshot_;
        status_->setText(UiLanguage::tr("Running"));
    } else if (event == "output") {
        output_->moveCursor(QTextCursor::End);
        output_->insertPlainText(body.value("output").toString());
    } else if (event == "terminated") {
        status_->setText(UiLanguage::tr("Program exited"));
        stop();
    }
}

void DebuggerPanel::configureBreakpoints(const QJsonObject& points, bool initial) {
    const auto configured = [this] {
        request("setFunctionBreakpoints", {{"breakpoints", QJsonArray{QJsonObject{{"name", "main"}}}}},
                [this](const QJsonObject&) { request("configurationDone"); });
    };
    QSet<QString> paths;
    for (auto it = points_.begin(); it != points_.end(); ++it)
        paths.insert(it.key());
    for (auto it = points.begin(); it != points.end(); ++it)
        paths.insert(it.key());
    points_ = points;
    if (paths.isEmpty()) {
        if (initial)
            configured();
        return;
    }
    const auto remaining = std::make_shared<int>(paths.size());
    for (const auto& path : paths) {
        QJsonArray lines;
        for (const auto& line : points.value(path).toArray())
            lines.append(QJsonObject{{"line", line}});
        request("setBreakpoints", {{"source", QJsonObject{{"path", path}}}, {"breakpoints", lines}},
                [initial, remaining, configured](const QJsonObject&) {
                    if (--*remaining == 0 && initial)
                        configured();
                });
    }
}

void DebuggerPanel::loadStack() {
    const auto revision = ++snapshot_;
    request(
        "stackTrace", {{"threadId", thread_}, {"startFrame", 0}, {"levels", 64}},
        [this, revision](const QJsonObject& body) {
            if (!paused_ || revision != snapshot_)
                return;
            frames_ = body.value("stackFrames").toArray();
            stack_->blockSignals(true);
            stack_->clear();
            for (const auto& value : frames_) {
                const auto frame = value.toObject();
                stack_->addItem(
                    QString("%1 · %2:%3")
                        .arg(frame.value("name").toString(),
                             QFileInfo(frame.value("source").toObject().value("path").toString()).fileName())
                        .arg(frame.value("line").toInt()));
            }
            stack_->setCurrentRow(0);
            stack_->blockSignals(false);
            loadFrame(0);
        });
}

void DebuggerPanel::loadFrame(int index) {
    if (!paused_ || index < 0 || index >= frames_.size())
        return;
    selected_ = index;
    const auto revision = ++snapshot_;
    const auto frame = frames_[index].toObject();
    variables_->clear();
    request("scopes", {{"frameId", frame.value("id")}}, [this, revision, frame](const QJsonObject& body) {
        if (!paused_ || revision != snapshot_)
            return;
        QList<int> references;
        for (const auto& value : body.value("scopes").toArray()) {
            const auto scope = value.toObject();
            if (!scope.value("expensive").toBool() && scope.value("variablesReference").toInt() > 0)
                references << scope.value("variablesReference").toInt();
        }
        const auto showLocation = [this, frame] {
            const auto path = frame.value("source").toObject().value("path").toString();
            if (QFileInfo::exists(path) && location_)
                location_(path, frame.value("line").toInt());
        };
        if (references.isEmpty()) {
            showLocation();
            return;
        }
        auto remaining = std::make_shared<int>(references.size());
        for (const int reference : references)
            request("variables", {{"variablesReference", reference}, {"count", 300}},
                    [this, revision, remaining, showLocation](const QJsonObject& values) {
                        if (!paused_ || revision != snapshot_)
                            return;
                        for (const auto& value : values.value("variables").toArray()) {
                            const auto variable = value.toObject();
                            if (variables_->topLevelItemCount() >= 300)
                                break;
                            new QTreeWidgetItem(variables_, {variable.value("name").toString(),
                                                             variable.value("value").toString(),
                                                             variable.value("type").toString()});
                        }
                        if (--*remaining == 0)
                            showLocation();
                    });
    });
}

void DebuggerPanel::send(QJsonObject command) {
    if (!ready_ || !busy())
        return;
    const auto name = command.value("command").toString();
    if (name == "frame") {
        loadFrame(command.value("index").toInt());
    } else if (name == "evaluate") {
        const int index = command.value("frame").toInt(selected_);
        if (!paused_ || index < 0 || index >= frames_.size())
            return;
        request(
            "evaluate",
            {{"expression", command.value("expression")},
             {"frameId", frames_[index].toObject().value("id")},
             {"context", "watch"}},
            [this](const QJsonObject& body) { output_->appendPlainText(body.value("result").toString()); });
    } else if (name == "stdin") {
#ifdef Q_OS_WIN
        target_.write(command.value("text").toString().toUtf8() + '\n');
#else
        input_ += command.value("text").toString().toUtf8() + '\n';
        writeTerminal();
#endif
    } else if (name == "breakpoints") {
        setBreakpoints(command.value("breakpoints").toObject());
    } else if (name == "stop") {
        stop();
    } else if (name == "continue" || name == "next" || name == "stepIn" || name == "stepOut" ||
               name == "pause") {
        if (name != "pause") {
            paused_ = false;
            ++snapshot_;
        }
        request(name, {{"threadId", thread_}});
    }
}

void DebuggerPanel::setBreakpoints(const QJsonObject& points) {
    if (ready_ && busy())
        configureBreakpoints(points, false);
}

void DebuggerPanel::stop() {
    if (stopping_ || !busy())
        return;
    stopping_ = true;
    paused_ = false;
    ++snapshot_;
    request("disconnect", {{"terminateDebuggee", true}});
    const auto generation = generation_;
    QTimer::singleShot(1500, this, [this, generation] {
        if (generation == generation_) {
            targetTree_.terminate();
            if (target_.state() != QProcess::NotRunning)
                target_.kill();
            if (busy()) {
                tree_.terminate();
                process_.kill();
            }
        }
    });
}

void DebuggerPanel::readTerminal() {
#ifndef Q_OS_WIN
    if (terminal_ < 0)
        return;
    char buffer[8192];
    ssize_t count;
    while ((count = ::read(terminal_, buffer, sizeof(buffer))) > 0) {
        output_->moveCursor(QTextCursor::End);
        output_->insertPlainText(terminalDecoder_(QByteArrayView(buffer, count)));
    }
#endif
}

void DebuggerPanel::writeTerminal() {
#ifndef Q_OS_WIN
    while (terminal_ >= 0 && !input_.isEmpty()) {
        const auto count = ::write(terminal_, input_.constData(), input_.size());
        if (count > 0)
            input_.remove(0, count);
        else if (count < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    if (terminalWriter_)
        terminalWriter_->setEnabled(!input_.isEmpty());
#endif
}

void DebuggerPanel::closeTerminal() {
#ifndef Q_OS_WIN
    delete terminalReader_;
    delete terminalWriter_;
    terminalReader_ = terminalWriter_ = nullptr;
    if (terminal_ >= 0)
        ::close(terminal_);
    if (terminalAnchor_ >= 0)
        ::close(terminalAnchor_);
    terminalAnchor_ = -1;
    terminal_ = -1;
#endif
    input_.clear();
    terminalDecoder_ = QStringDecoder(QStringDecoder::Utf8);
}
