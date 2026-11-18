#pragma once
#include "ts/LanguageTools.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

namespace ToolDiscovery {
struct Sdk {
    QString name;
    QString root;
    QString variable;
};

inline QList<Sdk> sdks() {
    QSettings prefs;
    QList<Sdk> result;
    const int count = prefs.beginReadArray("sdk/entries");
    for (int i = 0; i < count; ++i) {
        prefs.setArrayIndex(i);
        result.push_back({prefs.value("name").toString(), prefs.value("root").toString(),
                          prefs.value("variable").toString()});
    }
    prefs.endArray();
    return result;
}

inline QJsonObject project(const QString& root = {}) {
    const auto directory = root.isEmpty() ? QSettings().value("workspace/root").toString() : root;
    if (directory.isEmpty())
        return {};
    QFile file(QDir(directory).filePath(".veyra/project.json"));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

inline Sdk projectSdk(const QString& root = {}) {
    const auto sdk = project(root).value("sdk").toObject();
    return {sdk.value("name").toString(), sdk.value("root").toString(), sdk.value("variable").toString()};
}

inline QList<Sdk> effectiveSdks() {
    const auto selected = projectSdk();
    return selected.root.isEmpty() ? sdks() : QList<Sdk>{selected};
}

inline QStringList sdkPaths(const Sdk& sdk) {
    return {QDir(sdk.root).filePath("bin"), sdk.root};
}

inline QString sdkExecutable(const QString& name, const QString& root = {}) {
    const auto selected = projectSdk(root);
    return selected.root.isEmpty() ? QString() : QStandardPaths::findExecutable(name, sdkPaths(selected));
}

inline QStringList paths() {
    QStringList paths;
    const auto append = [&](const QString& path) {
        if (QDir(path).exists() && !paths.contains(path))
            paths << QDir(path).absolutePath();
    };
    for (const auto& sdk : effectiveSdks()) {
        if (sdk.root.isEmpty())
            continue;
        append(QDir(sdk.root).filePath("bin"));
        append(sdk.root);
    }
    for (const auto& path :
         QString::fromLocal8Bit(qgetenv("PATH")).split(QDir::listSeparator(), Qt::SkipEmptyParts))
        append(path);
    const auto home = QDir::homePath();
    for (const auto& path :
         QStringList{home + "/.local/bin", home + "/.cargo/bin", home + "/.dotnet", "/opt/homebrew/bin",
                     "/usr/local/bin", "/usr/local/go/bin", "/usr/local/share/dotnet"})
        append(path);
    return paths;
}

inline QString find(const QString& name) {
    return QStandardPaths::findExecutable(name, paths());
}

inline QString compiler(const QString& language, const QString& fallback, const QString& root = {}) {
    if (projectSdk(root).root.isEmpty())
        return find(QSettings().value("toolchains/" + language + "/build", fallback).toString());
    const auto configured =
        project(root).value("tools").toObject().value(language).toObject().value("build").toString();
    if (!configured.isEmpty())
        return QStandardPaths::findExecutable(configured);
    QStringList names{fallback};
    for (const auto& profile : ts::availableLanguages())
        if (profile.lspId == language.toStdString() && !profile.compilerCandidates.empty()) {
            names.clear();
            for (const auto& name : profile.compilerCandidates)
                names << QString::fromStdString(name);
        }
    for (const auto& name : names) {
        const auto path = sdkExecutable(name, root);
        if (!path.isEmpty())
            return path;
    }
    return {};
}

inline QString server(const QString& command) {
    const auto local = QDir(QCoreApplication::applicationDirPath()).filePath("../Resources/tools/" + command);
    if (QFileInfo(local).isExecutable())
        return QFileInfo(local).absoluteFilePath();
    return find(command);
}

struct DetectedSdk {
    ts::LanguageInfo language;
    QString compiler, runtime, server, root;
};

inline QList<DetectedSdk> detect(const QString& directory = {}) {
    auto profiles = ts::availableLanguages();
    QFile manifest(QDir(directory).filePath("veyra-languages.json"));
    if (!directory.isEmpty() && manifest.open(QIODevice::ReadOnly) && manifest.size() <= 4 * 1024 * 1024)
        for (const auto& value :
             QJsonDocument::fromJson(manifest.readAll()).object().value("languages").toArray()) {
            const auto definition = value.toObject();
            ts::LanguageInfo profile;
            profile.lspId = definition.value("key").toString().toStdString();
            profile.name = definition.value("name").toString().toStdString();
            profile.suggestedServer = definition.value("server").toString().toStdString();
            profile.sdkVariable = definition.value("sdkVariable").toString().toStdString();
            profile.installation =
                QJsonDocument(definition.value("installation").toObject()).toJson().toStdString();
            for (const auto& item : definition.value("extensions").toArray())
                profile.extensions.pushBack(item.toString().toStdString());
            for (const auto& item : definition.value("compilerCandidates").toArray())
                profile.compilerCandidates.pushBack(item.toString().toStdString());
            const auto program = definition.value("build").toObject().value("program").toString();
            if (!program.isEmpty() && !program.contains('{'))
                profile.compilerCandidates.pushBack(program.toStdString());
            for (const auto& item : definition.value("sdkExecutables").toArray())
                profile.sdkExecutables.pushBack(item.toString().toStdString());
            if (!profile.lspId.empty() && !profile.name.empty())
                profiles.pushBack(profile);
        }
    const auto rootsFor = [](QString pattern) {
        pattern.replace("{home}", QDir::homePath());
        const QRegularExpression environment("\\{env:([A-Za-z_][A-Za-z0-9_]*)\\}");
        for (;;) {
            const auto match = environment.match(pattern);
            if (!match.hasMatch())
                break;
            const auto value = qEnvironmentVariable(match.captured(1).toUtf8().constData());
            if (value.isEmpty())
                return QStringList{};
            pattern.replace(match.capturedStart(), match.capturedLength(), value);
        }
        pattern = QDir::fromNativeSeparators(pattern);
        if (!QDir::isAbsolutePath(pattern))
            return QStringList{};
        auto parts = pattern.split('/', Qt::SkipEmptyParts);
        QStringList paths{QDir::rootPath()};
#ifdef Q_OS_WIN
        if (!parts.isEmpty() && parts[0].endsWith(':')) {
            paths = {parts.takeFirst() + '/'};
        }
#endif
        for (const auto& part : parts) {
            QStringList next;
            for (const auto& path : paths) {
                if (part.contains('*') || part.contains('?')) {
                    const auto directories = QDir(path).entryList({part}, QDir::Dirs | QDir::NoDotAndDotDot);
                    for (const auto& entry : directories)
                        if (next.size() < 256)
                            next << QDir(path).filePath(entry);
                } else if (QDir(QDir(path).filePath(part)).exists())
                    next << QDir(path).filePath(part);
            }
            paths = next;
        }
        paths.removeDuplicates();
        return paths;
    };
    QList<DetectedSdk> result;
    for (const auto& profile : profiles) {
        QStringList roots;
        if (!directory.isEmpty())
            roots << directory;
        else {
            for (const auto& pattern : profile.sdkRoots)
                roots.append(rootsFor(QString::fromStdString(pattern)));
            roots.removeDuplicates();
            roots << QString();
        }
        bool specific = false;
        for (const auto& candidateRoot : roots) {
            if (candidateRoot.isEmpty() && specific)
                continue;
            const auto search = [&](const QString& name) {
                return candidateRoot.isEmpty()
                           ? find(name)
                           : QStandardPaths::findExecutable(name, sdkPaths({{}, candidateRoot, {}}));
            };
            const auto sample =
                "sample." + (profile.extensions.empty() ? std::string("txt") : profile.extensions[0]);
            const auto plan = ts::buildPlanFor(sample, "output");
            QString compiler =
                plan.build.valid ? search(QString::fromStdString(plan.build.program)) : QString();
            if (compiler.isEmpty())
                for (const auto& name : profile.compilerCandidates) {
                    compiler = search(QString::fromStdString(name));
                    if (!compiler.isEmpty())
                        break;
                }
            auto runtime = plan.run.valid && (!plan.compiled || plan.run.program != plan.outputPath)
                               ? search(QString::fromStdString(plan.run.program))
                               : QString();
            if (runtime.isEmpty() && !compiler.isEmpty() && !candidateRoot.isEmpty() && plan.run.valid &&
                plan.run.program != plan.outputPath)
                runtime = find(QString::fromStdString(plan.run.program));
            if (compiler.isEmpty() && runtime.isEmpty())
                for (const auto& name : profile.sdkExecutables) {
                    runtime = search(QString::fromStdString(name));
                    if (!runtime.isEmpty())
                        break;
                }
            const auto words = QProcess::splitCommand(QString::fromStdString(profile.suggestedServer));
            auto languageServer = words.isEmpty() ? QString() : search(words[0]);
            if (languageServer.isEmpty() && !words.isEmpty())
                languageServer = find(words[0]);
            if (compiler.isEmpty() && runtime.isEmpty() &&
                (candidateRoot.isEmpty() ? languageServer.isEmpty() : true))
                continue;
            const auto tool = !compiler.isEmpty() ? compiler : !runtime.isEmpty() ? runtime : languageServer;
            const auto canonical = QFileInfo(tool).canonicalFilePath();
            const auto binaryDirectory = QFileInfo(canonical.isEmpty() ? tool : canonical).dir();
            const auto inferred = binaryDirectory.dirName().compare("bin", Qt::CaseInsensitive) == 0
                                      ? binaryDirectory.absoluteFilePath("..")
                                      : binaryDirectory.absolutePath();
            const auto root =
                candidateRoot.isEmpty() ? QDir(inferred).absolutePath() : QDir(candidateRoot).absolutePath();
            bool valid = true;
            for (const auto& marker : profile.sdkRequiredFiles)
                valid = valid && QFileInfo::exists(QDir(root).filePath(QString::fromStdString(marker)));
            if (!valid)
                continue;
            result.push_back({profile, compiler, runtime, languageServer, root});
            if (!candidateRoot.isEmpty())
                specific = true;
        }
    }
    return result;
}

inline bool adopt(const DetectedSdk& chosen, const QString& projectRoot, QString& error) {
    const auto manifest = QDir(chosen.root).filePath("veyra-languages.json");
    if (QFileInfo::exists(manifest)) {
        std::string issue;
        if (!ts::importLanguageProfiles(manifest.toStdString(), issue)) {
            error = QString::fromStdString(issue);
            return false;
        }
    }
    const auto id = QString::fromStdString(chosen.language.lspId);
    QJsonObject sdk{{"name", QString::fromStdString(chosen.language.name)},
                    {"root", chosen.root},
                    {"variable", QString::fromStdString(chosen.language.sdkVariable)}};
    if (!projectRoot.isEmpty()) {
        auto metadata = project(projectRoot);
        metadata["sdk"] = sdk;
        auto tools = metadata.value("tools").toObject();
        tools[id] = QJsonObject{{"build", chosen.compiler}, {"run", chosen.runtime}};
        metadata["tools"] = tools;
        QDir().mkpath(QDir(projectRoot).filePath(".veyra"));
        QSaveFile file(QDir(projectRoot).filePath(".veyra/project.json"));
        const auto content = QJsonDocument(metadata).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit()) {
            error = file.errorString();
            return false;
        }
    }
    auto entries = sdks();
    bool exists = false;
    for (const auto& entry : entries)
        exists = exists || entry.root == chosen.root;
    if (!exists)
        entries.push_front({QString::fromStdString(chosen.language.name), chosen.root,
                            QString::fromStdString(chosen.language.sdkVariable)});
    QSettings preferences;
    preferences.beginWriteArray("sdk/entries", entries.size());
    for (int index = 0; index < entries.size(); ++index) {
        preferences.setArrayIndex(index);
        preferences.setValue("name", entries[index].name);
        preferences.setValue("root", entries[index].root);
        preferences.setValue("variable", entries[index].variable);
    }
    preferences.endArray();
    if (!chosen.compiler.isEmpty())
        preferences.setValue("toolchains/" + id + "/build", chosen.compiler);
    if (!chosen.runtime.isEmpty())
        preferences.setValue("toolchains/" + id + "/run", chosen.runtime);
    if (!chosen.server.isEmpty()) {
        auto command = QProcess::splitCommand(QString::fromStdString(chosen.language.suggestedServer));
        command[0] = chosen.server;
        for (auto& word : command)
            word = "\"" + word.replace("\"", "\\\"") + "\"";
        preferences.setValue("lsp/" + id + "/command", command.join(' '));
    }
    preferences.setValue("sdk/discoveryReviewed", true);
    return true;
}

inline QProcessEnvironment environment() {
    auto environment = QProcessEnvironment::systemEnvironment();
    const auto selected = effectiveSdks();
    for (auto it = selected.crbegin(); it != selected.crend(); ++it)
        if (!it->variable.isEmpty() && !it->root.isEmpty())
            environment.insert(it->variable, it->root);
    environment.insert("PATH", paths().join(QDir::listSeparator()));
    environment.insert("DOTNET_CLI_TELEMETRY_OPTOUT", "1");
    environment.insert("DOTNET_NOLOGO", "1");
    return environment;
}
}
