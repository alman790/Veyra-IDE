#include "CMakeFileApi.hpp"
#include "../gui/ToolDiscovery.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace {
QJsonObject read(const QString& path, QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024) {
        error = "CMake response is unavailable or too large: " + path;
        return {};
    }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        error = "Invalid File API response: " + path;
        return {};
    }
    return document.object();
}

ts::ProcessSpec command(const QString& program, const QString& directory, const QStringList& args) {
    ts::ProcessSpec result;
    result.valid = true;
    result.program = program.toStdString();
    result.workingDirectory = directory.toStdString();
    for (const auto& value : args)
        result.arguments.pushBack(value.toStdString());
    return result;
}

QString cmake() {
    return QSettings().value("toolchains/cmake", "cmake").toString();
}
}

bool CMakeFileApi::prepareQuery(const QString& build, QString& error) {
    const auto directory = QDir(build).filePath(".cmake/api/v1/query/client-veyra");
    if (!QDir().mkpath(directory)) {
        error = "Could not create the File API query.";
        return false;
    }
    QFile file(directory + "/codemodel-v2");
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
        return false;
    }
    error.clear();
    return true;
}

QList<CMakeTarget> CMakeFileApi::readTargets(const QString& build, const QString& wanted, QString& error) {
    error.clear();
    const QDir reply(QDir(build).filePath(".cmake/api/v1/reply"));
    const auto indexes = reply.entryList({"index-*.json"}, QDir::Files, QDir::Name | QDir::Reversed);
    if (indexes.isEmpty()) {
        error = "CMake did not produce a File API response. Run Configure.";
        return {};
    }
    const auto index = read(reply.filePath(indexes.first()), error);
    if (!error.isEmpty())
        return {};
    QString model;
    for (const auto& item : index.value("objects").toArray()) {
        const auto object = item.toObject();
        if (object.value("kind") == "codemodel" &&
            object.value("version").toObject().value("major").toInt() == 2)
            model = object.value("jsonFile").toString();
    }
    if (model.isEmpty() || model.contains('/') || model.contains('\\')) {
        error = "The codemodel v2 response is missing.";
        return {};
    }
    const auto data = read(reply.filePath(model), error);
    if (!error.isEmpty())
        return {};
    QJsonObject chosen;
    for (const auto& value : data.value("configurations").toArray()) {
        const auto config = value.toObject();
        if (config.value("name").toString() == wanted || config.value("name").toString().isEmpty()) {
            chosen = config;
            break;
        }
    }
    if (chosen.isEmpty()) {
        error = "Configuration " + wanted + " is missing from the File API.";
        return {};
    }
    const auto targets = chosen.value("targets").toArray();
    if (targets.size() > 2000) {
        error = "File API inspection limit: 2,000 targets.";
        return {};
    }
    QList<CMakeTarget> result;
    for (const auto& value : targets) {
        const auto object = value.toObject();
        const auto json = object.value("jsonFile").toString();
        if (json.isEmpty() || json.contains('/') || json.contains('\\')) {
            error = "Invalid File API target file.";
            return {};
        }
        const auto target = read(reply.filePath(json), error);
        if (!error.isEmpty())
            return {};
        CMakeTarget record;
        record.name = target.value("name").toString();
        record.type = target.value("type").toString();
        record.configuration = wanted;
        const auto artifacts = target.value("artifacts").toArray();
        if (record.type == "EXECUTABLE" && !artifacts.isEmpty()) {
            const auto path = artifacts.first().toObject().value("path").toString();
            record.artifact =
                QDir::cleanPath(QFileInfo(path).isAbsolute() ? path : QDir(build).absoluteFilePath(path));
        }
        result.append(record);
    }
    return result;
}

ts::ProcessSpec CMakeFileApi::configureCommand(const CMakeSettings& settings) {
    QStringList args{"-S",
                     settings.root,
                     "-B",
                     settings.build,
                     "-DCMAKE_BUILD_TYPE=" + settings.configuration,
                     "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"};
    if (!ToolDiscovery::projectSdk(settings.root).root.isEmpty()) {
        const auto language = ToolDiscovery::project(settings.root).value("language").toString("cpp");
        ts::LanguageInfo profile;
        for (const auto& candidate : ts::availableLanguages())
            if (candidate.lspId == language.toStdString())
                profile = candidate;
        const auto plan = ts::buildPlanFor(
            "main." + (profile.extensions.empty() ? std::string() : profile.extensions[0]), "output");
        const auto compiler =
            ToolDiscovery::compiler(language, QString::fromStdString(plan.build.program), settings.root);
        if (compiler.isEmpty() || profile.cmakeLanguage.empty())
            return {};
        args << "-DCMAKE_" + QString::fromStdString(profile.cmakeLanguage) + "_COMPILER=" + compiler;
    }
    if (!settings.generator.trimmed().isEmpty())
        args << "-G" << settings.generator.trimmed();
    return command(cmake(), settings.root, args);
}

ts::ProcessSpec CMakeFileApi::buildCommand(const CMakeSettings& settings, const QString& target) {
    QStringList args{"--build", settings.build, "--config", settings.configuration, "--parallel", "2"};
    if (!target.isEmpty())
        args << "--target" << target;
    return command(cmake(), settings.root, args);
}

ts::ProcessSpec CMakeFileApi::runCommand(const CMakeSettings& settings, const CMakeTarget& target) {
    return command(target.artifact, settings.root, {});
}
