#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <iostream>

static void emitMessage(const QJsonObject& object) {
    std::cout << "VEYRA_AGENT " << QJsonDocument(object).toJson(QJsonDocument::Compact).constData()
              << std::endl;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    emitMessage({{"type", "ready"}, {"version", 1}});
    std::string line;
    int step = 0;
    while (std::getline(std::cin, line)) {
        const auto request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        if (request.value("method") == "providers") {
            emitMessage({{"type", "providers"},
                         {"providers",
                          QJsonArray{QJsonObject{{"name", "test-provider"}, {"model", "fixture-model"}}}}});
            continue;
        }
        if (request.value("method") != "chat")
            continue;
        const auto mode = qEnvironmentVariable("VEYRA_AGENT_FIXTURE");
        if (mode == "wait") {
            QThread::msleep(30000);
            continue;
        }
        const auto id = request.value("id");
        if (mode == "invalid") {
            emitMessage({{"type", "response"}, {"id", id}, {"content", "not a structured response"}});
            continue;
        }
        QJsonObject response;
        if (++step == 1)
            response = {{"action", "read"}, {"path", "Counter.hpp"}, {"line", 1}};
        else if (step == 2)
            response = {{"action", "search"}, {"query", "Counter::next"}};
        else {
            const QJsonArray edits{
                QJsonObject{
                    {"path", "Counter.cpp"}, {"before", "return missing;"}, {"after", "return value;"}},
                QJsonObject{
                    {"path", "Counter.hpp"}, {"before", "int value = 7;"}, {"after", "int value = 8;"}}};
            response = {{"action", "propose"},
                        {"summary", "Use the declared member and update its initial value."},
                        {"edits", edits}};
        }
        emitMessage({{"type", "response"},
                     {"id", id},
                     {"content", QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact))}});
    }
}
