#include "CodeRunner.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <stdexcept>

static void check(bool ok, const QString& message) {
    if (!ok)
        throw std::runtime_error(message.toStdString());
}

static void waitFor(const std::function<bool()>& predicate, int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    check(predicate(), "Asynchronous process timeout");
}

static QByteArray read(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        check(argc == 2, "Expected fixture binary");
        QTemporaryDir temp;
        const QString directory = temp.path() + "/проект с пробелами";
        check(QDir().mkpath(directory), "Create Unicode directory");
        const QString source = directory + "/исходный файл.cpp";
        QFile file(source);
        check(file.open(QIODevice::WriteOnly), "Create C++ source");
        file.write("#include <iostream>\nint main(){std::cout << \"VEYRA_NATIVE_OK: ");
        file.write(QString("Привет").toUtf8());
        file.write("\";return 0;}\n");
        file.close();
        CodeRunner runner;
        QString output, error;
        bool done = false, success = false;
        int code = -1, notifications = 0;
        auto finished = [&](bool ok, int status) {
            done = true;
            success = ok;
            code = status;
            ++notifications;
        };
        auto printed = [&](const QString& text) { output += text; };
        check(runner.start(source, true, printed, finished, error), error);
        waitFor([&] { return done; }, 120000);
        check(success && code == 0 && output.contains("VEYRA_NATIVE_OK: Привет"),
              "Native UTF-8 compile/run and final output draining");
        check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Rewrite compiler error fixture");
        file.write("int main( {");
        file.close();
        done = false;
        output.clear();
        check(runner.start(source, false, printed, finished, error), error);
        waitFor([&] { return done; }, 120000);
        check(!success && code != 0 && output.contains("error"), "Compiler diagnostics");
        ts::ProcessSpec fixture;
        fixture.valid = true;
        fixture.program = app.arguments()[1].toStdString();
        fixture.workingDirectory = directory.toStdString();
        const QString heartbeat = directory + "/heartbeat.txt";
        fixture.arguments.pushBack("--tree");
        fixture.arguments.pushBack(heartbeat.toStdString());
        ts::Array<ts::ProcessSpec> commands;
        commands.pushBack(fixture);
        done = false;
        output.clear();
        check(runner.startCommands(commands, true, printed, finished, error), error);
        waitFor([&] { return output.contains("CHILD_READY") && !read(heartbeat).isEmpty(); }, 10000);
        runner.stop();
        waitFor([&] { return done; }, 5000);
        check(!success && code == 130 && !runner.busy(), "Stop waits for actual process termination");
        const auto stopped = read(heartbeat);
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 250) {
            app.processEvents();
            QThread::msleep(5);
        }
        check(read(heartbeat) == stopped, "Stop terminates descendant execution");
        done = false;
        output.clear();
        runner.setTimeouts(100000, 200);
        check(runner.startCommands(commands, true, printed, finished, error), error);
        waitFor([&] { return done; }, 10000);
        check(!success && code == 124, "Timeout terminates process tree");
        runner.setTimeouts(100000, 10000);
        fixture.arguments.clear();
        fixture.arguments.pushBack("--input");
        fixture.arguments.pushBack("unused");
        commands.clear();
        commands.pushBack(fixture);
        done = false;
        output.clear();
        check(runner.startCommands(commands, true, printed, finished, error), error);
        waitFor([&] { return output.contains("INPUT_READY"); }, 10000);
        runner.sendInput("hello stdin");
        waitFor([&] { return done; }, 10000);
        check(success && output.contains("INPUT_RECEIVED:hello stdin"),
              "Interactive stdin reaches running program");
        fixture.program = "veyra_missing_tool_8f71";
        commands.clear();
        commands.pushBack(fixture);
        error.clear();
        check(!runner.startCommands(commands, true, printed, finished, error) &&
                  error.contains("veyra_missing_tool_8f71"),
              "Actionable missing tool error");
        check(notifications == 5, "Exactly one completion for each successful start");
        std::cout << "PASS: real compiler, Unicode/spaces, stderr, final output, Stop/timeout descendant "
                     "cleanup and missing tools\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
