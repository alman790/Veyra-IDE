#include "DebuggerPanel.hpp"
#include "ToolDiscovery.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (DebuggerPanel::adapterPath().isEmpty() || ToolDiscovery::find("c++").isEmpty())
        return 77;
    QTemporaryDir temporary;
    try {
        const auto file = temporary.path() + "/main.cpp", executable = temporary.path() + "/program";
        QFile source(file);
        if (!source.open(QIODevice::WriteOnly))
            return 1;
        source.write("#include <iostream>\nint main() {\n    int value = 41;\n    value += 1;\n    std::cout "
                     "<< \"RESULT=\" << value << '\\n';\n"
                     "    char input[128]; std::cin.getline(input, sizeof(input));\n"
                     "    std::cout << \"ECHO=\" << input << '\\n';\n    return 0;\n}\n");
        source.close();
        QProcess compiler;
        compiler.start(ToolDiscovery::find("c++"), {"-g", "-O0", file, "-o", executable});
        if (!compiler.waitForFinished(30000) || compiler.exitCode() != 0)
            throw std::runtime_error("Build debug fixture");
        int line = 0;
        DebuggerPanel panel(nullptr, [&](const QString&, int value) { line = value; });
        panel.show();
        QString error;
        if (!panel.start(executable, temporary.path(), {{file, QJsonArray{4}}}, error))
            throw std::runtime_error(error.toStdString());
        const auto wait = [&](const std::function<bool()>& ready, int milliseconds) {
            QElapsedTimer deadline;
            deadline.start();
            while (!ready() && deadline.elapsed() < milliseconds)
                QTest::qWait(20);
            if (!ready())
                throw std::runtime_error(
                    panel.findChild<QPlainTextEdit*>("debugConsole")->toPlainText().toStdString() +
                    "\nDebugger timed out");
        };
        wait([&] { return line > 0; }, 15000);
        panel.send({{"command", "continue"}});
        wait([&] { return line == 4; }, 10000);
        auto* variables = panel.findChild<QTreeWidget*>("debugVariables");
        bool found = false;
        for (int index = 0; index < variables->topLevelItemCount(); ++index) {
            auto* value = variables->topLevelItem(index);
            if (value->text(0) == "value" && value->text(1) == "41")
                found = true;
        }
        if (!found)
            throw std::runtime_error("Paused locals must contain value = 41");
        panel.send({{"command", "next"}});
        wait([&] { return line == 5; }, 10000);
        panel.send({{"command", "evaluate"}, {"expression", "value"}});
        wait([&] { return panel.findChild<QPlainTextEdit*>("debugConsole")->toPlainText().contains("42"); },
             10000);
        panel.send({{"command", "continue"}});
        panel.send({{"command", "stdin"}, {"text", QString::fromUtf8("Привет")}});
        wait([&] { return !panel.busy(); }, 10000);
        if (!panel.findChild<QPlainTextEdit*>("debugConsole")->toPlainText().contains("RESULT=42"))
            throw std::runtime_error("Capture debuggee stdout");
        if (!panel.findChild<QPlainTextEdit*>("debugConsole")
                 ->toPlainText()
                 .contains(QString::fromUtf8("ECHO=Привет")))
            throw std::runtime_error("Round-trip UTF-8 program input through the native terminal");
        line = 0;
        if (!panel.start(executable, temporary.path(), {{file, QJsonArray{4}}}, error))
            throw std::runtime_error(error.toStdString());
        wait([&] { return line > 0; }, 10000);
        QTest::qWait(1200);
        if (!panel.busy())
            throw std::runtime_error("An old stop deadline must not kill a restarted debug session");
        panel.stop();
        wait([&] { return !panel.busy(); }, 10000);
        std::cout << "PASS: real LLDB launch, breakpoint, stack, locals, stepping, evaluation and shutdown\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
