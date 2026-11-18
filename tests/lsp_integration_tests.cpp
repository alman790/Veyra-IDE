#include "JsonRpcFramer.hpp"
#include "LspClient.hpp"
#include "ToolDiscovery.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <stdexcept>

static void check(bool ok, const QString& message) {
    if (!ok)
        throw std::runtime_error(message.toStdString());
}

static void waitFor(const std::function<bool()>& ready, int seconds) {
    QElapsedTimer time;
    time.start();
    while (!ready() && time.elapsed() < seconds * 1000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    check(ready(), "Real clangd response timeout");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        JsonRpcFramer framer;
        QString error;
        int messages = 0;
        const QJsonObject original{{"jsonrpc", "2.0"}, {"id", 7}, {"text", QString::fromUtf8("Привет 🔥")}};
        const auto bytes = JsonRpcFramer::frame(original);
        for (char byte : bytes)
            check(framer.feed(
                      QByteArray(1, byte),
                      [&](const auto& received) {
                          check(received == original, "UTF-8 framed payload");
                          ++messages;
                      },
                      error),
                  error);
        check(messages == 1, "Fragmented frame dispatch exactly once");
        check(!framer.feed(
                  "Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}", [](const auto&) {}, error),
              "Reject duplicate length");
        check(
            !framer.feed("Content-Length: -1\r\n\r\n", [](const auto&) {}, error), "Reject negative length");
        check(!framer.feed("Content-Length: 1\r\n\r\n[", [](const auto&) {}, error), "Reject malformed JSON");
        const auto server = QStandardPaths::findExecutable("clangd");
        if (server.isEmpty()) {
            std::cout << "SKIP: real clangd is not installed; framing checks passed\n";
            return 77;
        }
        QTemporaryDir temp;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path() + "/settings");
        QCoreApplication::setOrganizationName("VeyraTest");
        QCoreApplication::setApplicationName("RealClangd");
        QSettings().setValue("lsp/cpp/command", "\"" + server + "\" --background-index=false --log=error");
        const QString first = temp.path() + "/источник с пробелами.cpp", second = temp.path() + "/second.cpp";
        QString code = "#include <string>\nint main(){ /*🔥*/ std::string value; value. }\n";
        for (const auto& path : {first, second}) {
            QFile file(path);
            check(file.open(QIODevice::WriteOnly), "Create clangd source");
            file.write(code.toUtf8());
        }
        const auto compiler = QStandardPaths::findExecutable("clang++");
        if (!compiler.isEmpty()) {
            const auto sdkRoot = QFileInfo(compiler).dir().absoluteFilePath("..");
            QDir().mkpath(temp.path() + "/.veyra");
            QFile metadata(temp.path() + "/.veyra/project.json");
            check(metadata.open(QIODevice::WriteOnly), "Create SDK metadata");
            metadata.write(
                QJsonDocument(QJsonObject{{"language", "cpp"}, {"sdk", QJsonObject{{"root", sdkRoot}}}})
                    .toJson());
            metadata.close();
            QSettings().setValue("workspace/root", temp.path());
        }
        LspClient client;
        QString logs;
        QStringList completions;
        int diagnostics = 0, publications = 0, connections = 0;
        auto status = [&](const QString& value) {
            logs += value + '\n';
            if (value.startsWith("LSP connected"))
                ++connections;
        };
        auto completed = [&](const QStringList& values) { completions = values; };
        auto diagnosed = [&](const QString& message, int line, const QString&) {
            if (line > 0)
                logs += "diagnostic: " + message + "\n";
            if (line == 0) {
                diagnostics = 0;
                ++publications;
            } else
                ++diagnostics;
        };
        const auto language = ts::languageForPath(first.toStdString());
        check(client.startFile(first, code, language, status, completed, diagnosed),
              "Start installed clangd");
        waitFor([&] { return client.ready(); }, 20);
        const auto line = code.section('\n', 1, 1);
        const int column = line.indexOf("value.") + 6;
        client.requestCompletion(1, column);
        waitFor(
            [&] {
                for (const auto& word : completions)
                    if (word.contains("size"))
                        return true;
                return false;
            },
            30);
        bool size = false;
        for (const auto& word : completions)
            if (word.contains("size"))
                size = true;
        check(size, "Real semantic std::string completion at a UTF-16 position after emoji\n" +
                        completions.join(" | ") + "\n" + logs);
        waitFor([&] { return diagnostics > 0; }, 30);
        bool semanticReceived = false;
        check(!client.semanticTypes().isEmpty(), "Server supplies semantic token legend");
        client.requestDocument("textDocument/semanticTokens/full", -1, -1, {}, [&](const QJsonValue& value) {
            semanticReceived = !value.toObject().value("data").toArray().isEmpty();
        });
        waitFor([&] { return semanticReceived; }, 20);
        bool definitionReceived = false, renameReceived = false, formattingReceived = false;
        const QString symbolSource = "int compute(int value){return value+1;} int main(){return compute(3);}";
        QFile saved(first);
        check(saved.open(QIODevice::WriteOnly | QIODevice::Truncate),
              "Save symbol fixture before cross-file rename");
        saved.write(symbolSource.toUtf8());
        saved.close();
        client.changeText(symbolSource);
        client.saveFile(first, symbolSource);
        client.requestDocument("textDocument/definition", 0, symbolSource.lastIndexOf("compute") + 2, {},
                               [&](const QJsonValue& value) { definitionReceived = !value.isNull(); });
        waitFor([&] { return definitionReceived; }, 20);
        client.requestDocument("textDocument/rename", 0, symbolSource.lastIndexOf("compute") + 2,
                               {{"newName", "increment"}}, [&](const QJsonValue& value) {
                                   renameReceived = value.toObject().contains("changes") ||
                                                    value.toObject().contains("documentChanges");
                               });
        waitFor([&] { return renameReceived; }, 20);
        client.requestDocument("textDocument/formatting", -1, -1,
                               {{"options", QJsonObject{{"tabSize", 4}, {"insertSpaces", true}}}},
                               [&](const QJsonValue& value) { formattingReceived = value.isArray(); });
        waitFor([&] { return formattingReceived; }, 20);
        const int before = publications;
        client.changeText("int main(){int okay=0;return okay;}\n");
        waitFor([&] { return publications > before && diagnostics == 0; }, 30);
        check(client.startFile(second, "int helper(){return undefined_name;}\n", language, status, completed,
                               diagnosed),
              "Activate second LSP document");
        waitFor([&] { return diagnostics > 0; }, 30);
        check(connections == 1, "Multiple documents reuse one language server");
        client.closeFile(second);
        client.stop();
        waitFor([&] { return !client.active(); }, 10);
        QDir().mkpath(temp.path() + "/include");
        QDir().mkpath(temp.path() + "/build");
        QFile header(temp.path() + "/include/library.hpp");
        check(header.open(QIODevice::WriteOnly), "Create custom library header");
        header.write("struct Library { int uniqueFunction(int value); };\n");
        header.close();
        QFile database(temp.path() + "/build/compile_commands.json");
        check(database.open(QIODevice::WriteOnly), "Create compilation database");
        const QJsonArray commands{QJsonObject{
            {"directory", temp.path()},
            {"file", first},
            {"arguments", QJsonArray{"clang++", "-std=c++20", "-I", temp.path() + "/include", "-c", first}}}};
        database.write(QJsonDocument(commands).toJson());
        database.close();
        QSettings().setValue("lsp/cpp/compileCommands", temp.path() + "/build");
        completions.clear();
        const QString custom = "#include <library.hpp>\nint main(){ Library object; object.uni; }\n";
        check(client.startFile(first, custom, language, status, completed, diagnosed),
              "Start clangd with compilation database");
        waitFor([&] { return client.ready(); }, 20);
        client.requestCompletion(1, custom.section('\n', 1, 1).indexOf("object.uni") + 10);
        waitFor(
            [&] {
                for (const auto& word : completions)
                    if (word.contains("uniqueFunction"))
                        return true;
                return false;
            },
            30);
        client.stop();
        waitFor([&] { return !client.active(); }, 10);
        QSettings().setValue("lsp/cpp/command", "veyra_missing_clangd_2a");
        check(!client.startFile(first, code, language, status, completed, diagnosed) &&
                  logs.contains("required server not found"),
              "Missing LSP keeps editor operational with an explanation");
        std::cout << "PASS: byte-fragmented JSON-RPC, malformed frames, real clangd "
                     "initialization/completion/diagnostics, UTF-16, multiple documents and shutdown\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
