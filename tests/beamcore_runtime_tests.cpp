#include "BeamcoreClient.hpp"
#include "ToolDiscovery.hpp"
#include "UiLanguage.hpp"
#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <iostream>
#include <memory>
#include <stdexcept>

static void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}

static bool waitFor(const std::function<bool()>& predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 15000)
        QTest::qWait(10);
    return predicate();
}

static QByteArray read(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Read isolated Beamcore configuration");
    return file.readAll();
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("VeyraTests");
    app.setApplicationName("BeamcoreRuntime");
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    const auto executable = BeamcoreClient::executable();
    if (executable.isEmpty()) {
        std::cout << "SKIP: Beamcore release runtime is not installed\n";
        return 77;
    }
    try {
        UiLanguage::initialize();
        QTcpServer server;
        check(server.listen(QHostAddress::LocalHost, 0), "Start local API fixture");
        int apiCalls = 0;
        QString requestText;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto* socket = server.nextPendingConnection();
            auto data = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, data] {
                *data += socket->readAll();
                const auto separator = data->indexOf("\r\n\r\n");
                if (separator < 0)
                    return;
                const QRegularExpression length("content-length:\\s*(\\d+)",
                                                QRegularExpression::CaseInsensitiveOption);
                const int size = length.match(QString::fromUtf8(data->left(separator))).captured(1).toInt();
                if (data->size() < separator + 4 + size)
                    return;
                ++apiCalls;
                requestText = QString::fromUtf8(data->mid(separator + 4, size));
                const QJsonObject body{
                    {"id", "fixture-completion"},
                    {"object", "chat.completion"},
                    {"created", 1},
                    {"model", "fixture-model"},
                    {"choices",
                     QJsonArray{QJsonObject{
                         {"index", 0},
                         {"finish_reason", "stop"},
                         {"message",
                          QJsonObject{{"role", "assistant"},
                                      {"content",
                                       "{\"action\":\"answer\",\"text\":\"External fixture response\"}"}}}}}},
                    {"usage",
                     QJsonObject{{"prompt_tokens", 10}, {"completion_tokens", 10}, {"total_tokens", 20}}}};
                const auto bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: "
                              "close\r\nContent-Length: " +
                              QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes);
                socket->disconnectFromHost();
            });
        });
        const auto config = temporary.filePath("original.dets");
        QProcess setup;
        auto environment = ToolDiscovery::environment();
        environment.insert("VEYRA_TEST_CONFIG", config);
        environment.insert("VEYRA_TEST_URL", QString("http://127.0.0.1:%1/v1").arg(server.serverPort()));
        setup.setProcessEnvironment(environment);
        setup.start(
            executable,
            {"eval",
             "Application.ensure_all_started(:openai_ex); "
             "Application.put_env(:beamcore, :config_dets_path, System.fetch_env!(\"VEYRA_TEST_CONFIG\")); "
             "{:ok, _} = Beamcore.Config.start_link(); "
             ":ok = Beamcore.Config.put_provider(\"veyra-fixture\", %{\"base_url\" => "
             "System.fetch_env!(\"VEYRA_TEST_URL\"), "
             "\"auth\" => \"none\", \"default_model\" => \"fixture-model\"}); "
             "GenServer.stop(Beamcore.Config)"});
        check(setup.waitForFinished(15000) && setup.exitCode() == 0,
              "Create isolated provider configuration through the real Beamcore API");
        const auto before = QCryptographicHash::hash(read(config), QCryptographicHash::Sha256);
        QSettings().setValue("agent/beamcoreExecutable", executable);
        QSettings().setValue("agent/beamcoreConfig", config);
        QSettings().setValue("agent/providerStore", temporary.filePath("private/providers.dets"));
        BeamcoreClient client;
        QJsonArray providers;
        QString error, answer;
        QObject::connect(&client, &BeamcoreClient::providersReceived, &client,
                         [&](const QJsonArray& value) { providers = value; });
        QObject::connect(&client, &BeamcoreClient::failed, &client,
                         [&](const QString& value) { error = value; });
        QObject::connect(&client, &BeamcoreClient::responseReceived, &client,
                         [&](const QString& value) { answer = value; });
        client.requestProviders();
        check(waitFor([&] { return !providers.isEmpty() || !error.isEmpty(); }) && error.isEmpty(),
              "Read providers through the real release runtime");
        check(providers.size() == 1 && providers[0].toObject().value("name") == "veyra-fixture",
              "Only isolated providers are exposed");
        client.chat(QJsonArray{QJsonObject{{"role", "user"}, {"content", "compiler-context-fixture"}}},
                    "veyra-fixture", "fixture-model");
        check(waitFor([&] { return !answer.isEmpty() || !error.isEmpty(); }) && error.isEmpty() &&
                  answer.contains("External fixture response"),
              "Beamcore's actual provider API calls the selected endpoint");
        check(apiCalls == 1 && requestText.contains("compiler-context-fixture") &&
                  requestText.contains("fixture-model"),
              "The selected model and project context reach the provider");
        answer.clear();
        bool nativeInference = false;
        QObject::connect(&client, &BeamcoreClient::inferenceRequested, &client,
                         [&](int id, const QJsonArray& messages, int) {
                             nativeInference = !messages.isEmpty();
                             client.inferenceResult(
                                 id, "{\"action\":\"answer\",\"text\":\"Native fixture response\"}");
                         });
        client.chat(QJsonArray{QJsonObject{{"role", "user"}, {"content", "local-project-context"}}}, {}, {});
        check(waitFor([&] { return !answer.isEmpty() || !error.isEmpty(); }) && error.isEmpty() &&
                  nativeInference && answer.contains("Native fixture response") && apiCalls == 1,
              "Local bridge requests native inference without calling a network provider");
        providers = {};
        client.saveProvider({{"name", "veyra-added"},
                             {"endpoint", QString("http://127.0.0.1:%1/v1").arg(server.serverPort())},
                             {"model", "added-model"},
                             {"auth", "bearer"},
                             {"api_key", "private-fixture-token"}});
        check(waitFor([&] { return providers.size() == 2 || !error.isEmpty(); }) && error.isEmpty(),
              "Create an API provider directly through the IDE bridge");
        check(!QJsonDocument(providers).toJson().contains("private-fixture-token"),
              "Provider listings never expose authentication secrets");
        client.stop();
        check(waitFor([&] { return !client.running(); }), "Stop provider runtime before reopening it");
        providers = {};
        client.requestProviders();
        check(waitFor([&] { return providers.size() == 2 || !error.isEmpty(); }) && error.isEmpty(),
              "IDE provider settings persist across runtime restarts");
        check(!read(temporary.filePath("private/providers.dets")).contains("private-fixture-token"),
              "Provider credentials are encrypted on disk");
        providers = {};
        client.removeProvider("veyra-added");
        check(waitFor([&] { return providers.size() == 1 || !error.isEmpty(); }) && error.isEmpty(),
              "Remove an IDE provider without changing the imported configuration");
        client.stop();
        check(QCryptographicHash::hash(read(config), QCryptographicHash::Sha256) == before,
              "Beamcore's original provider configuration remains byte-for-byte unchanged");
        std::cout << "PASS: real Beamcore release, provider API, native inference transport and "
                     "configuration isolation\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
