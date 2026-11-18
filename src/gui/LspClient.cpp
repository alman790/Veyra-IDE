#include "LspClient.hpp"
#include "ToolDiscovery.hpp"
#include "ts/ProjectTools.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>

namespace {
QString uri(const QString& path) {
    return QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString(QUrl::FullyEncoded);
}
}

LspClient::LspClient(QObject* parent) : QObject(parent) {
    completionRetry_.setSingleShot(true);
    completionRetry_.setInterval(450);
    connect(&completionRetry_, &QTimer::timeout, this, [this] {
        if (!ready() || completionUri_ != documentUri_ || !documents_.contains(documentUri_) ||
            documents_[documentUri_].version != completionVersion_)
            return;
        const int remaining = completionRetries_;
        requestCompletion(completionLine_, completionColumn_);
        completionRetries_ = remaining - 1;
    });
    shutdownDeadline_.setSingleShot(true);
    connect(&shutdownDeadline_, &QTimer::timeout, this, [this] { server_.kill(); });
    connect(&server_, &QProcess::started, this, &LspClient::initialize);
    connect(&server_, &QProcess::readyReadStandardError, this, [this] {
        const auto text = QString::fromUtf8(server_.readAllStandardError()).left(8192);
        if (status_ && !text.isEmpty())
            status_("LSP: " + text.trimmed());
    });
    connect(&server_, &QProcess::readyReadStandardOutput, this, [this] {
        QString error;
        if (!framer_.feed(
                server_.readAllStandardOutput(), [this](const auto& message) { accept(message); }, error)) {
            if (status_)
                status_("LSP protocol error: " + error);
            server_.kill();
        }
    });
    connect(&server_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (status_)
            status_("LSP: " + server_.errorString());
    });
    connect(&server_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
                shutdownDeadline_.stop();
                initialized_ = false;
                stopping_ = false;
                documents_.clear();
                requests_.clear();
                framer_.clear();
                completionRequest_ = -1;
                if (status_)
                    status_(QString("LSP stopped (exit %1)").arg(code));
                if (!destroying_ && pendingStart_) {
                    auto start = std::move(pendingStart_);
                    QTimer::singleShot(0, this, std::move(start));
                }
            });
}

LspClient::~LspClient() {
    destroying_ = true;
    pendingStart_ = {};
    status_ = {};
    completions_ = {};
    diagnostic_ = {};
    completionRetry_.stop();
    shutdownDeadline_.stop();
    server_.disconnect(this);
    if (active()) {
        server_.kill();
        server_.waitForFinished(1500);
    }
}

void LspClient::send(const QJsonObject& request) {
    if (server_.state() == QProcess::Running)
        server_.write(JsonRpcFramer::frame(request));
}

void LspClient::notification(const QString& method, const QJsonObject& params) {
    send({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
}

void LspClient::initialize() {
    initializeRequest_ = nextRequest_++;
    const auto root = uri(workspace_);
    const QJsonObject capabilities{
        {"general", QJsonObject{{"positionEncodings", QJsonArray{"utf-16"}}}},
        {"textDocument",
         QJsonObject{
             {"completion", QJsonObject{{"completionItem", QJsonObject{{"snippetSupport", false}}}}},
             {"publishDiagnostics", QJsonObject{{"versionSupport", true}}},
             {"semanticTokens",
              QJsonObject{{"requests", QJsonObject{{"full", true}}},
                          {"tokenTypes",
                           QJsonArray{"namespace",  "type",          "class",     "enum",     "interface",
                                      "struct",     "typeParameter", "parameter", "variable", "property",
                                      "enumMember", "function",      "method",    "macro",    "keyword",
                                      "modifier",   "comment",       "string",    "number",   "operator"}},
                          {"tokenModifiers", QJsonArray{}},
                          {"formats", QJsonArray{"relative"}},
                          {"overlappingTokenSupport", false},
                          {"multilineTokenSupport", false}}}}}};
    send({{"jsonrpc", "2.0"},
          {"id", initializeRequest_},
          {"method", "initialize"},
          {"params", QJsonObject{{"processId", static_cast<qint64>(QCoreApplication::applicationPid())},
                                 {"rootUri", root},
                                 {"capabilities", capabilities},
                                 {"initializationOptions", initializationOptions_},
                                 {"clientInfo", QJsonObject{{"name", "Veyra IDE"}, {"version", "2.0"}}}}}});
}

void LspClient::opened(const QString& name) {
    auto it = documents_.find(name);
    if (!ready() || it == documents_.end() || it->opened)
        return;
    notification("textDocument/didOpen", {{"textDocument", QJsonObject{{"uri", name},
                                                                       {"languageId", it->language},
                                                                       {"version", it->version},
                                                                       {"text", it->text}}}});
    it->opened = true;
}

void LspClient::cancelCompletion() {
    completionRetry_.stop();
    if (completionRequest_ >= 0)
        notification("$/cancelRequest", {{"id", completionRequest_}});
    completionRequest_ = -1;
}

void LspClient::accept(const QJsonObject& message) {
    const QString method = message.value("method").toString();
    if (!method.isEmpty() && message.contains("id")) {
        QJsonObject reply{{"jsonrpc", "2.0"}, {"id", message.value("id")}};
        if (method == "workspace/configuration") {
            QJsonArray result;
            for (const auto& item : message.value("params").toObject().value("items").toArray()) {
                const auto section = item.toObject().value("section").toString();
                QJsonValue value(serverSettings_);
                for (const auto& part : section.split('.', Qt::SkipEmptyParts))
                    value = value.toObject().value(part);
                result.append(value.isUndefined() ? QJsonValue(QJsonValue::Null) : value);
            }
            reply["result"] = result;
        } else if (method == "client/registerCapability" || method == "window/workDoneProgress/create")
            reply["result"] = QJsonValue::Null;
        else if (method == "workspace/applyEdit")
            reply["result"] =
                QJsonObject{{"applied", false}, {"failureReason", "Workspace edits are not enabled"}};
        else
            reply["error"] = QJsonObject{{"code", -32601}, {"message", "Method not implemented"}};
        send(reply);
        return;
    }
    if (method == "textDocument/publishDiagnostics") {
        const auto params = message.value("params").toObject();
        const auto name = params.value("uri").toString();
        if (!documents_.contains(name))
            return;
        if (params.contains("version") && params.value("version").toInt() != documents_[name].version)
            return;
        documents_[name].parsedVersion = documents_[name].version;
        documents_[name].diagnostics = params.value("diagnostics").toArray();
        publishCached(name);
        return;
    }
    if (!message.value("id").isDouble())
        return;
    const int id = message.value("id").toInt(-1);
    if (id == shutdownRequest_) {
        notification("exit", {});
        return;
    }
    if (id == initializeRequest_ && !stopping_) {
        if (message.contains("error")) {
            if (status_)
                status_("LSP initialization rejected: " +
                        message.value("error").toObject().value("message").toString());
            stop();
            return;
        }
        const auto result = message.value("result").toObject();
        semanticTypes_.clear();
        const auto semantic =
            result.value("capabilities").toObject().value("semanticTokensProvider").toObject();
        if (semantic.value("full").toBool() || semantic.value("full").isObject())
            for (const auto& value : semantic.value("legend").toObject().value("tokenTypes").toArray())
                semanticTypes_ << value.toString();

        positionEncoding_ = result.value("capabilities").toObject().value("positionEncoding").toString();
        if (positionEncoding_.isEmpty())
            positionEncoding_ = result.value("offsetEncoding").toString("utf-16");
        if (status_)
            status_("LSP position encoding: " + positionEncoding_);
        initialized_ = true;
        notification("initialized", {});
        notification("workspace/didChangeConfiguration", {{"settings", serverSettings_}});
        for (auto it = documents_.begin(); it != documents_.end(); ++it)
            opened(it.key());
        if (status_)
            status_("LSP connected: UTF-16 positions, completion and diagnostics");
        return;
    }
    auto pending = requests_.find(id);
    if (pending != requests_.end()) {
        const auto request = std::move(pending.value());
        requests_.erase(pending);
        if (request.uri == documentUri_ && documents_.contains(request.uri) &&
            documents_[request.uri].version == request.version) {
            if (message.contains("error")) {
                if (status_)
                    status_("Language action failed: " +
                            message.value("error").toObject().value("message").toString());
            } else
                request.callback(message.value("result"));
        }
        return;
    }
    if (id == completionRequest_) {
        completionRequest_ = -1;
        if (!completions_ || completionUri_ != documentUri_ || !documents_.contains(documentUri_) ||
            completionVersion_ != documents_[documentUri_].version)
            return;
        const auto result = message.value("result");
        const auto items = result.isArray() ? result.toArray() : result.toObject().value("items").toArray();
        QStringList words;
        completionEdits_.clear();
        for (const auto& item : items) {
            const auto object = item.toObject();
            QString text = object.value("textEdit").toObject().value("newText").toString();
            if (text.isEmpty())
                text = object.value("insertText").toString();
            if (text.isEmpty())
                text = object.value("label").toString();
            if (!text.isEmpty() && !words.contains(text) && words.size() < 200) {
                words << text;
                const auto edit = object.value("textEdit").toObject();
                if (!edit.isEmpty()) {
                    auto details = edit;
                    details["additionalTextEdits"] = object.value("additionalTextEdits");
                    completionEdits_[text] = details;
                }
            }
        }
        completions_(words);
        const bool awaitingParse = (languageId_ == "cpp" || languageId_ == "c") &&
                                   documents_[documentUri_].parsedVersion != completionVersion_;
        if (!awaitingParse && !words.isEmpty() && result.toObject().value("isIncomplete").toBool())
            completionRetries_ = std::min(completionRetries_, 3);
        if (completionRetries_ > 0 &&
            (awaitingParse || words.isEmpty() || result.toObject().value("isIncomplete").toBool()))
            completionRetry_.start();
    }
}

void LspClient::publishCached(const QString& name) {
    if (name != documentUri_ || !diagnostic_ || !documents_.contains(name))
        return;
    diagnostic_("", 0, "");
    for (const auto& entry : documents_[name].diagnostics) {
        const auto diagnostic = entry.toObject();
        const auto start = diagnostic.value("range").toObject().value("start").toObject();
        diagnostic_((diagnostic.value("severity").toInt() == 1   ? "[error] "
                     : diagnostic.value("severity").toInt() == 2 ? "[warning] "
                                                                 : "[info] ") +
                        diagnostic.value("message").toString(),
                    start.value("line").toInt() + 1, diagnostic.value("source").toString("LSP"));
    }
}

bool LspClient::startFile(const QString& path, const QString& text, const ts::LanguageInfo& language,
                          std::function<void(const QString&)> status,
                          std::function<void(const QStringList&)> completions,
                          std::function<void(const QString&, int, const QString&)> diagnostic) {
    if (path.isEmpty())
        return false;
    const auto key = "lsp/" + QString::fromStdString(language.lspId) + "/command";
    QString command = QSettings().value(key).toString().trimmed();
    if (command.isEmpty())
        command = QString::fromStdString(language.suggestedServer);
    auto words = QProcess::splitCommand(command);
    const auto root = ts::findCMakeRoot(path.toStdString());
    QString workspace = root.empty() ? QFileInfo(path).absolutePath() : QString::fromStdString(root);
    const auto projectRoot = QSettings().value("workspace/root").toString();
    if (!projectRoot.isEmpty() &&
        QFileInfo(path).absoluteFilePath().startsWith(QDir(projectRoot).absolutePath() + '/'))
        workspace = QDir(projectRoot).absolutePath();
    if (!words.isEmpty() && QFileInfo(words.first()).baseName().startsWith("clangd")) {
        QString database = QSettings().value("lsp/cpp/compileCommands").toString();
        if (database.isEmpty() && !root.empty()) {
            const auto prefix =
                "cmake/project/" +
                QString::fromLatin1(
                    QCryptographicHash::hash(workspace.toUtf8(), QCryptographicHash::Sha256).toHex());
            database = QSettings().value(prefix + "/build").toString();
        }
        const auto sdk = ToolDiscovery::projectSdk();
        if (!sdk.root.isEmpty()) {
            const bool c = language.id == ts::LanguageId::C;
            const auto compiler = ToolDiscovery::compiler(c ? "c" : "cpp", c ? "cc" : "c++");
            if (compiler.isEmpty()) {
                pendingStart_ = {};
                stop();
                if (status)
                    status("LSP: selected SDK has no compatible C/C++ compiler");
                return false;
            }
            words << "--query-driver=" + compiler;
            // Before the first CMake configure, give clangd the selected compiler too.
            {
                QJsonArray inherited;
                QFile existing(QDir(database).filePath("compile_commands.json"));
                if (!database.isEmpty() && existing.open(QIODevice::ReadOnly) &&
                    existing.size() <= 16 * 1024 * 1024)
                    inherited = QJsonDocument::fromJson(existing.readAll()).array();
                for (int index = 0; index < inherited.size(); ++index) {
                    auto entry = inherited[index].toObject();
                    auto arguments = entry.value("arguments").toArray();
                    if (arguments.isEmpty())
                        arguments = QJsonArray::fromStringList(
                            QProcess::splitCommand(entry.value("command").toString()));
                    if (!arguments.isEmpty())
                        arguments[0] = compiler;
                    entry.remove("command");
                    entry["arguments"] = arguments;
                    inherited[index] = entry;
                }
                database =
                    QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/lsp/" +
                    QString::fromLatin1(QCryptographicHash::hash((workspace + sdk.root + compiler).toUtf8(),
                                                                 QCryptographicHash::Sha256)
                                            .toHex());
                QDir().mkpath(database);
                QFile prior(QDir(database).filePath("compile_commands.json"));
                QJsonArray entries = inherited;
                if (entries.isEmpty() && prior.open(QIODevice::ReadOnly) && prior.size() < 16 * 1024 * 1024)
                    entries = QJsonDocument::fromJson(prior.readAll()).array();
                prior.close();
                if (inherited.isEmpty()) {
                    for (int index = entries.size() - 1; index >= 0; --index)
                        if (entries[index].toObject().value("file").toString() == path)
                            entries.removeAt(index);
                    entries.append(QJsonObject{
                        {"directory", workspace},
                        {"file", path},
                        {"arguments", QJsonArray{compiler, c ? "-std=c17" : "-std=c++20", "-c", path}}});
                }
                QSaveFile file(QDir(database).filePath("compile_commands.json"));
                const auto content = QJsonDocument(entries).toJson();
                if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() ||
                    !file.commit()) {
                    pendingStart_ = {};
                    stop();
                    if (status)
                        status("LSP: unable to write SDK compilation database");
                    return false;
                }
            }
        }
        if (!database.isEmpty() && QFileInfo::exists(QDir(database).filePath("compile_commands.json"))) {
            bool explicitArgument = false;
            for (const auto& argument : words)
                if (argument.startsWith("--compile-commands-dir"))
                    explicitArgument = true;
            if (!explicitArgument)
                words << "--compile-commands-dir=" + QDir(database).absolutePath();
        }
    }
    auto environmentValues = ToolDiscovery::environment().toStringList();
    environmentValues.sort();
    const auto environmentKey =
        QCryptographicHash::hash(environmentValues.join('\n').toUtf8(), QCryptographicHash::Sha256);
    command =
        QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(words)).toJson(QJsonDocument::Compact));
    if (active() && (command != command_ || workspace != workspace_ || environmentKey != environmentKey_ ||
                     languageId_ != QString::fromStdString(language.lspId) || stopping_)) {
        documentUri_.clear();
        pendingStart_ = [=, this] { startFile(path, text, language, status, completions, diagnostic); };
        stop();
        return true;
    }
    status_ = std::move(status);
    completions_ = std::move(completions);
    diagnostic_ = std::move(diagnostic);
    if (words.isEmpty() || ToolDiscovery::server(words.first()).isEmpty()) {
        if (status_)
            status_("LSP disabled: required server not found: " +
                    (words.isEmpty() ? QString::fromStdString(language.name) : words.first()));
        return false;
    }
    cancelCompletion();
    documentUri_ = uri(path);
    languageId_ = QString::fromStdString(language.lspId);
    if (documents_.contains(documentUri_)) {
        changeText(text);
        opened(documentUri_);
    } else {
        documents_.insert(documentUri_, Document{text, languageId_, 1, false, {}});
        opened(documentUri_);
    }
    publishCached(documentUri_);
    if (active())
        return true;
    const auto profilePlan = ts::buildPlanFor(
        "file." + (language.extensions.empty() ? std::string("txt") : language.extensions[0]),
        ".veyra/build");
    const auto sdk = ToolDiscovery::projectSdk();
    const auto compiler = ToolDiscovery::compiler(QString::fromStdString(language.lspId),
                                                  QString::fromStdString(profilePlan.build.program));
    const std::function<QJsonValue(const QJsonValue&)> expand = [&](const QJsonValue& value) -> QJsonValue {
        if (value.isString())
            return value.toString().replace("{sdk}", sdk.root).replace("{compiler}", compiler);
        if (value.isArray()) {
            QJsonArray array;
            for (const auto& item : value.toArray())
                array.append(expand(item));
            return array;
        }
        if (value.isObject()) {
            auto object = value.toObject();
            for (auto it = object.begin(); it != object.end(); ++it)
                it.value() = expand(it.value());
            return object;
        }
        return value;
    };
    serverSettings_ =
        expand(QJsonDocument::fromJson(QByteArray::fromStdString(language.serverSettings)).object())
            .toObject();
    initializationOptions_ =
        expand(QJsonDocument::fromJson(QByteArray::fromStdString(language.initializationOptions)).object())
            .toObject();
    command_ = command;
    environmentKey_ = environmentKey;
    workspace_ = workspace;
    initialized_ = false;
    stopping_ = false;
    nextRequest_ = 1;
    shutdownRequest_ = -1;
    server_.setWorkingDirectory(workspace_);
    server_.setProcessChannelMode(QProcess::SeparateChannels);
    server_.setProcessEnvironment(ToolDiscovery::environment());
    server_.start(ToolDiscovery::server(words.first()), words.mid(1));
    return true;
}

void LspClient::changeText(const QString& text) {
    auto it = documents_.find(documentUri_);
    if (it == documents_.end() || it->text == text)
        return;
    cancelCompletion();
    for (auto request = requests_.begin(); request != requests_.end(); ++request)
        notification("$/cancelRequest", {{"id", request.key()}});
    requests_.clear();
    it->text = text;
    ++it->version;
    it->diagnostics = {};
    if (ready() && it->opened)
        notification("textDocument/didChange",
                     {{"textDocument", QJsonObject{{"uri", documentUri_}, {"version", it->version}}},
                      {"contentChanges", QJsonArray{QJsonObject{{"text", text}}}}});
}

void LspClient::saveFile(const QString& path, const QString& text) {
    const auto name = uri(path);
    if (!documents_.contains(name) || !ready())
        return;
    if (name == documentUri_)
        changeText(text);
    notification("textDocument/didSave", {{"textDocument", QJsonObject{{"uri", name}}}, {"text", text}});
}

void LspClient::closeFile(const QString& path) {
    const auto name = uri(path);
    if (documents_.contains(name) && documents_[name].opened)
        notification("textDocument/didClose", {{"textDocument", QJsonObject{{"uri", name}}}});
    documents_.remove(name);
    if (name == documentUri_) {
        cancelCompletion();
        documentUri_.clear();
    }
}

void LspClient::requestCompletion(int line, int column) {
    if (!ready() || !documents_.contains(documentUri_) || line < 0 || column < 0)
        return;
    cancelCompletion();
    completionLine_ = line;
    completionColumn_ = column;
    completionRetries_ = 30;
    completionRequest_ = nextRequest_++;
    completionUri_ = documentUri_;
    completionVersion_ = documents_[documentUri_].version;
    const auto sourceLine = documents_[documentUri_].text.section('\n', line, line);
    if (positionEncoding_ == "utf-8")
        column = sourceLine.left(column).toUtf8().size();
    else if (positionEncoding_ == "utf-32")
        column = sourceLine.left(column).toUcs4().size();
    send({{"jsonrpc", "2.0"},
          {"id", completionRequest_},
          {"method", "textDocument/completion"},
          {"params", QJsonObject{{"textDocument", QJsonObject{{"uri", documentUri_}}},
                                 {"position", QJsonObject{{"line", line}, {"character", column}}}}}});
}

void LspClient::stop() {
    if (stopping_ || !active())
        return;
    stopping_ = true;
    cancelCompletion();
    if (initialized_) {
        for (auto it = documents_.begin(); it != documents_.end(); ++it)
            if (it->opened)
                notification("textDocument/didClose", {{"textDocument", QJsonObject{{"uri", it.key()}}}});
        shutdownRequest_ = nextRequest_++;
        send({{"jsonrpc", "2.0"},
              {"id", shutdownRequest_},
              {"method", "shutdown"},
              {"params", QJsonValue::Null}});
    } else
        server_.terminate();
    initialized_ = false;
    shutdownDeadline_.start(1000);
}

void LspClient::requestDocument(const QString& method, int line, int column, const QJsonObject& extra,
                                std::function<void(const QJsonValue&)> callback) {
    if (!ready() || !documents_.contains(documentUri_))
        return;
    const int id = nextRequest_++;
    QJsonObject parameters = extra;
    parameters["textDocument"] = QJsonObject{{"uri", documentUri_}};
    if (line >= 0)
        parameters["position"] = QJsonObject{{"line", line}, {"character", column}};
    requests_.insert(id, {documentUri_, documents_[documentUri_].version, std::move(callback)});
    send({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", parameters}});
}

int LspClient::documentVersion(const QString& path) const {
    const auto found = documents_.constFind(uri(path));
    return found == documents_.cend() ? -1 : found->version;
}
