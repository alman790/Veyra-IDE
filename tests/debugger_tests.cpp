#include "DebuggerPanel.hpp"
#include "ToolDiscovery.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc == 2 && QByteArray(argv[1]) == "--help")
        return 9;
    QApplication app(argc, argv);
    app.setOrganizationName("VeyraTests");
    app.setApplicationName("NativeDebugger");
    QTemporaryDir settings, tools;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings().setValue("debugger/adapter", QCoreApplication::applicationFilePath());
    QString detail;
    if (!DebuggerPanel::adapterPath(&detail).isEmpty() || !detail.contains("(9)")) {
        std::cerr << "Reject an adapter that cannot start: " << detail.toStdString() << '\n';
        return 1;
    }
    QSettings().remove("debugger/adapter");
    auto invalid = tools.path() + "/lldb-dap";
#ifdef Q_OS_WIN
    invalid += ".exe";
#endif
    if (!QFile::copy(QCoreApplication::applicationFilePath(), invalid))
        return 1;
    const auto originalPath = qgetenv("PATH");
    qputenv("PATH", tools.path().toUtf8() + QDir::listSeparator().toLatin1() + originalPath);
    const auto adapter = DebuggerPanel::adapterPath(&detail);
    qputenv("PATH", originalPath);
    if (QFileInfo(adapter).canonicalFilePath() == QFileInfo(invalid).canonicalFilePath()) {
        std::cerr << "Auto-detection selected a broken adapter\n";
        return 1;
    }
    auto compilerPath = ToolDiscovery::find("c++");
#ifdef Q_OS_WIN
    const auto sdkCompiler = QStandardPaths::findExecutable("clang++", {QFileInfo(adapter).absolutePath()});
    if (!sdkCompiler.isEmpty())
        compilerPath = sdkCompiler;
#endif
    if (adapter.isEmpty() || compilerPath.isEmpty()) {
        std::cout << "SKIP: no working native debugger SDK: " << detail.toStdString() << '\n';
        return 77;
    }
    QTemporaryDir temporary;
    try {
        const auto file = temporary.path() + "/main.cpp";
        auto executable = temporary.path() + "/program";
#ifdef Q_OS_WIN
        executable += ".exe";
#endif
        QFile source(file);
        if (!source.open(QIODevice::WriteOnly))
            return 1;
        source.write("#include <iostream>\nint main() {\n    int value = 41;\n    value += 1;\n    std::cout "
                     "<< \"RESULT=\" << value << '\\n';\n"
                     "    char input[128]; std::cin.getline(input, sizeof(input));\n"
                     "    std::cout << \"ECHO=\" << input << '\\n';\n    return 0;\n}\n");
        source.close();
        QProcess compiler;
        compiler.start(compilerPath, {"-g", "-O0", file, "-o", executable});
        if (!compiler.waitForFinished(30000) || compiler.exitCode() != 0)
            throw std::runtime_error("Build debug fixture: " + compiler.readAllStandardError().toStdString());
        if (!QFileInfo::exists(executable))
            throw std::runtime_error("Compiler did not produce the debug executable");
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
                    "\nDebugger timed out\nAdapter: " + adapter.toStdString() +
                    "\nCompiler: " + compilerPath.toStdString());
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
