#include "JsonRpcFramer.hpp"
#include "LspClient.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <cstring>
#include <iostream>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

static void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

static bool waitFor(const std::function<bool()>& ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 10000)
        QTest::qWait(10);
    return ready();
}

static int serve() {
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    auto send = [](const QJsonObject& message) {
        const auto bytes = JsonRpcFramer::frame(message);
        std::cout.write(bytes.constData(), bytes.size());
        std::cout.flush();
    };
    for (;;) {
        std::string header;
        int length = 0;
        while (std::getline(std::cin, header)) {
            if (header.empty() || header == "\r")
                break;
            if (header.starts_with("Content-Length:"))
                length = std::stoi(header.substr(15));
        }
        if (!std::cin || length <= 0 || length > 1024 * 1024)
            return 0;
        std::string bytes(length, '\0');
        std::cin.read(bytes.data(), length);
        const auto request = QJsonDocument::fromJson(QByteArray::fromStdString(bytes)).object();
        const auto method = request.value("method").toString();
        const auto params = request.value("params").toObject();
        if (method == "initialize") {
            const auto root = QUrl(params.value("rootUri").toString()).path();
            if (root != "/srv/veyra project" || !params.value("processId").isNull())
                return 4;
            send({{"jsonrpc", "2.0"},
                  {"id", request.value("id")},
                  {"result",
                   QJsonObject{{"capabilities", QJsonObject{{"textDocumentSync", 1},
                                                            {"completionProvider", QJsonObject{}},
                                                            {"documentFormattingProvider", true}}}}}});
        } else if (method == "textDocument/didOpen") {
            const auto uri = params.value("textDocument").toObject().value("uri").toString();
            if (QUrl(uri).path() != QString::fromUtf8("/srv/veyra project/привет.cpp"))
                return 5;
            const QJsonObject position{{"line", 0}, {"character", 0}};
            send({{"jsonrpc", "2.0"},
                  {"method", "textDocument/publishDiagnostics"},
                  {"params", QJsonObject{{"uri", uri},
                                         {"version", 1},
                                         {"diagnostics",
                                          QJsonArray{QJsonObject{
                                              {"range", QJsonObject{{"start", position}, {"end", position}}},
                                              {"severity", 2},
                                              {"message", "remote fixture diagnostic"}}}}}}});
        } else if (method == "textDocument/completion") {
            send({{"jsonrpc", "2.0"},
                  {"id", request.value("id")},
                  {"result", QJsonArray{QJsonObject{{"label", "remoteSymbol"}}}}});
        } else if (method == "textDocument/formatting") {
            send({{"jsonrpc", "2.0"},
                  {"id", request.value("id")},
                  {"result", QJsonArray{QJsonObject{{"newText", "formatted"}}}}});
        } else if (method == "shutdown") {
            send({{"jsonrpc", "2.0"}, {"id", request.value("id")}, {"result", QJsonValue(QJsonValue::Null)}});
        } else if (method == "exit")
            return 0;
    }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "--lsp-server") == 0)
        return serve();
    app.setOrganizationName("VeyraTests");
    app.setApplicationName("RemoteLsp");
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    try {
        LspClient client;
        QStringList words;
        QString status;
        bool diagnosed = false;
        client.setTransport({QCoreApplication::applicationFilePath(),
                             "/srv/veyra project",
                             "/opt/remote-sdk",
                             {"--lsp-server"}});
        const auto path = QString::fromUtf8("/srv/veyra project/привет.cpp");
        require(client.startFile(
                    path, "int remoteSymbol;\n", ts::profileFor(ts::LanguageId::Cpp),
                    [&](const QString& value) { status = value; },
                    [&](const QStringList& value) { words = value; },
                    [&](const QString& value, int line, const QString&) {
                        if (line > 0 && value.contains("remote fixture"))
                            diagnosed = true;
                    }),
                "Start external language-server transport");
        require(waitFor([&] { return client.ready(); }),
                "Initialize with a remote POSIX URI and no local process ID");
        require(waitFor([&] { return diagnosed; }), "Receive remote diagnostics");
        client.requestCompletion(0, 3);
        require(waitFor([&] { return words.contains("remoteSymbol"); }),
                "Receive remote semantic completion");
        bool formatted = false;
        client.requestDocument("textDocument/formatting", -1, -1, {},
                               [&](const QJsonValue& value) { formatted = !value.toArray().isEmpty(); });
        require(waitFor([&] { return formatted; }), "Forward remote document actions");
        require(client.documentVersion(path) == 1,
                "Remote document lookup uses server paths on every client platform");
        client.stop();
        require(waitFor([&] { return !client.active(); }), "Stop external server transport cleanly");
        std::cout << "PASS: remote LSP initialization, POSIX URIs, diagnostics, completion, formatting and "
                     "shutdown\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
