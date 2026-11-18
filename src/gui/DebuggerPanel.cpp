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
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

DebuggerPanel::DebuggerPanel(QWidget* parent, std::function<void(const QString&, int)> location)
    : QWidget(parent), location_(std::move(location)) {
    tree_.configure(process_);
    connect(&process_, &QProcess::started, this, [this] {
        QString error;
        tree_.attach(process_.processId(), error);
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
    connect(&process_, &QProcess::errorOccurred, this, [this] { status_->setText(process_.errorString()); });
    connect(&process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) {
                tree_.reset();
                ready_ = false;
                status_->setText(UiLanguage::tr("Debug session ended"));
            });
}

DebuggerPanel::~DebuggerPanel() {
    process_.disconnect(this);
    tree_.terminate();
    if (busy()) {
        process_.terminate();
        if (!process_.waitForFinished(1500)) {
            process_.kill();
            process_.waitForFinished(500);
        }
    }
}

bool DebuggerPanel::start(const QString& executable, const QString& directory, const QJsonObject& breakpoints,
                          QString& error) {
    if (busy()) {
        error = "A debug session is already active.";
        return false;
    }
    const auto debugger = ToolDiscovery::find(QSettings().value("debugger/lldb", "lldb").toString());
    if (debugger.isEmpty()) {
        error = "Select an LLDB executable in Settings before debugging.";
        return false;
    }
    const QDir application(QCoreApplication::applicationDirPath());
    QFile source(TS_DEBUG_BRIDGE_PATH);
    for (const auto& relative : {"../Resources/tools/debug_bridge.py", "tools/debug_bridge.py",
                                 "../share/VeyraIDE/tools/debug_bridge.py"}) {
        const auto candidate = application.filePath(relative);
        if (QFileInfo::exists(candidate)) {
            source.setFileName(candidate);
            break;
        }
    }
    if (!source.open(QIODevice::ReadOnly)) {
        error = "The bundled debugger bridge is unavailable.";
        return false;
    }
    QFile script(QDir(scripts_.path()).filePath("debug_bridge.py"));
    if (!script.open(QIODevice::WriteOnly) || script.write(source.readAll()) < 0) {
        error = script.errorString();
        return false;
    }
    script.close();
    const auto path =
        QString::fromUtf8(QJsonDocument(QJsonArray{scripts_.path()}).toJson(QJsonDocument::Compact))
            .mid(1)
            .chopped(1);
    const auto command = "script import sys; sys.path.insert(0, " + path +
                         "); import debug_bridge; debug_bridge.run(lldb.debugger)";
    launch_ = {
        {"command", "launch"}, {"program", executable}, {"cwd", directory}, {"breakpoints", breakpoints}};
    pending_.clear();
    output_->clear();
    stack_->clear();
    variables_->clear();
    ready_ = false;
    process_.setProcessEnvironment(ToolDiscovery::environment());
    process_.setWorkingDirectory(directory);
    process_.start(debugger, {"--no-lldbinit", "--no-use-colors", "--batch", "-o", command});
    status_->setText(UiLanguage::tr("Starting debugger…"));
    return true;
}

void DebuggerPanel::consume() {
    pending_ += process_.readAllStandardOutput();
    for (;;) {
        const int newline = pending_.indexOf('\n');
        if (newline < 0)
            break;
        auto line = pending_.left(newline);
        pending_.remove(0, newline + 1);
        const int marker = line.indexOf("@@TS_DEBUG@@");
        if (marker >= 0)
            acceptMessage(QJsonDocument::fromJson(line.mid(marker + 12)).object());
        else if (!line.trimmed().isEmpty() && !line.startsWith("(lldb) script "))
            output_->appendPlainText(QString::fromUtf8(line));
    }
}

void DebuggerPanel::acceptMessage(const QJsonObject& message) {
    const auto type = message.value("event").toString();
    if (type == "ready") {
        ready_ = true;
        send(launch_);
    } else if (type == "stopped") {
        status_->setText(UiLanguage::tr("Paused"));
        stack_->blockSignals(true);
        stack_->clear();
        const auto frames = message.value("frames").toArray();
        for (const auto& value : frames) {
            const auto frame = value.toObject();
            stack_->addItem(
                QString("%1 · %2:%3")
                    .arg(frame.value("name").toString(), QFileInfo(frame.value("file").toString()).fileName())
                    .arg(frame.value("line").toInt()));
        }
        const int selected = message.value("selected").toInt();
        stack_->setCurrentRow(selected);
        stack_->blockSignals(false);
        variables_->clear();
        for (const auto& value : message.value("variables").toArray()) {
            const auto variable = value.toObject();
            new QTreeWidgetItem(variables_,
                                {variable.value("name").toString(), variable.value("value").toString(),
                                 variable.value("type").toString()});
        }
        if (selected < frames.size()) {
            const auto frame = frames[selected].toObject();
            if (QFileInfo::exists(frame.value("file").toString()) && location_)
                location_(frame.value("file").toString(), frame.value("line").toInt());
        }
    } else if (type == "running")
        status_->setText(UiLanguage::tr("Running"));
    else if (type == "output" || type == "evaluation" || type == "error") {
        output_->appendPlainText(message.value("text").toString());
        if (type == "error")
            status_->setText(UiLanguage::text(message.value("text").toString()));
    } else if (type == "exited") {
        status_->setText(UiLanguage::tr("Program exited"));
        stop();
    }
}

void DebuggerPanel::send(QJsonObject command) {
    if (ready_ && busy())
        process_.write(QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n');
}

void DebuggerPanel::setBreakpoints(const QJsonObject& points) {
    send({{"command", "breakpoints"}, {"breakpoints", points}});
}

void DebuggerPanel::stop() {
    send({{"command", "stop"}});
    const auto generation = generation_;
    QTimer::singleShot(1000, this, [this, generation] {
        if (generation == generation_ && busy()) {
            tree_.terminate();
            process_.kill();
        }
    });
}
