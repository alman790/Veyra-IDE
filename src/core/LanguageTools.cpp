#include "ts/LanguageTools.hpp"
#include "LanguageCatalog.hpp"
#include "ts/AtomicFile.hpp"
#include "ts/Path.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

namespace ts {
namespace {
namespace fs = std::filesystem;

struct Registry {
    std::vector<LanguageInfo> profiles;
    std::vector<QJsonObject> definitions;
};

Registry load() {
    QJsonArray entries =
        QJsonDocument::fromJson(QByteArray::fromRawData(reinterpret_cast<const char*>(TS_LANGUAGE_CATALOG),
                                                        sizeof(TS_LANGUAGE_CATALOG)))
            .object()
            .value("languages")
            .toArray();
    QString custom = qEnvironmentVariable("TS_LANGUAGE_PROFILES");
    if (custom.isEmpty() && QCoreApplication::instance())
        custom = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/languages.json";
    QFile file(custom);
    if (!custom.isEmpty() && file.open(QIODevice::ReadOnly) && file.size() <= 4 * 1024 * 1024) {
        for (const auto& value :
             QJsonDocument::fromJson(file.readAll()).object().value("languages").toArray()) {
            const auto key = value.toObject().value("key").toString();
            if (key.isEmpty())
                continue;
            bool replaced = false;
            for (int index = 0; index < entries.size(); ++index) {
                if (entries[index].toObject().value("key").toString() != key)
                    continue;
                auto merged = entries[index].toObject();
                const auto update = value.toObject();
                for (auto item = update.begin(); item != update.end(); ++item)
                    merged[item.key()] = item.value();
                entries[index] = merged;
                replaced = true;
                break;
            }
            if (!replaced)
                entries.append(value);
        }
    }
    Registry result;
    for (const auto& value : entries) {
        auto object = value.toObject();
#ifdef _WIN32
        const auto platform = object.value("windows").toObject();
        for (auto it = platform.begin(); it != platform.end(); ++it)
            object[it.key()] = it.value();
#endif
#ifdef _MSC_VER
        const auto compiler = object.value("msvc").toObject();
        for (auto it = compiler.begin(); it != compiler.end(); ++it)
            object[it.key()] = it.value();
#endif
        LanguageInfo profile;
        std::uint32_t identifier = 2166136261u;
        for (const auto byte : object.value("key").toString().toUtf8())
            identifier = (identifier ^ static_cast<unsigned char>(byte)) * 16777619u;
        int id = object.value("id").toInt(1000 + int(identifier & 0x3fffffff));
        if (id <= 0 || std::any_of(result.profiles.begin(), result.profiles.end(),
                                   [id](const auto& entry) { return int(entry.id) == id; }))
            id = 1000 + int(identifier & 0x3fffffff);
        profile.id = static_cast<LanguageId>(id);
        profile.name = object.value("name").toString().toStdString();
        profile.lspId = object.value("key").toString().toStdString();
        profile.suggestedServer = object.value("server").toString().toStdString();
        profile.runnable = object.value("runnable").toBool();
        profile.preparation = object.value("preparation").toString().toStdString();
        profile.sourceTemplate = object.value("template").toString().toStdString();
        profile.entry = object.value("entry").toString().toStdString();
        profile.sdkVariable = object.value("sdkVariable").toString().toStdString();
        profile.lineComment = object.value("lineComment").toString().toStdString();
        profile.serverSettings = QJsonDocument(object.value("serverSettings").toObject())
                                     .toJson(QJsonDocument::Compact)
                                     .toStdString();
        profile.initializationOptions = QJsonDocument(object.value("initializationOptions").toObject())
                                            .toJson(QJsonDocument::Compact)
                                            .toStdString();
        profile.installation = QJsonDocument(object.value("installation").toObject())
                                   .toJson(QJsonDocument::Compact)
                                   .toStdString();
        profile.cmakeLanguage = object.value("cmakeLanguage").toString().toStdString();
        for (const auto& value : object.value("highlightPatterns").toArray())
            profile.highlightPatterns.pushBack({value.toObject().value("pattern").toString().toStdString(),
                                                value.toObject().value("role").toString().toStdString()});
        profile.blockComments = object.value("blockComments").toBool();
        const auto append = [](const QJsonArray& values, Array<std::string>& output) {
            for (const auto& item : values)
                output.pushBack(item.toString().toStdString());
        };
        append(object.value("extensions").toArray(), profile.extensions);
        append(object.value("keywords").toArray(), profile.keywords);
        append(object.value("compilerCandidates").toArray(), profile.compilerCandidates);
        append(object.value("sdkExecutables").toArray(), profile.sdkExecutables);
        append(object.value("sdkRoots").toArray(), profile.sdkRoots);
        append(object.value("sdkRequiredFiles").toArray(), profile.sdkRequiredFiles);
        append(object.value("quotePrefixes").toArray(), profile.quotePrefixes);
        const auto files = object.value("projectFiles").toObject();
        for (auto it = files.begin(); it != files.end(); ++it)
            profile.projectFiles.pushBack({it.key().toStdString(), it.value().toString().toStdString()});
        if (profile.name.empty() || profile.lspId.empty() || profile.extensions.empty())
            continue;
        result.profiles.push_back(std::move(profile));
        result.definitions.push_back(object);
    }
    return result;
}

std::shared_ptr<const Registry>& storage() {
    static std::shared_ptr<const Registry> instance = std::make_shared<Registry>(load());
    return instance;
}

std::shared_ptr<const Registry> registry() {
    return std::atomic_load(&storage());
}

std::string expand(std::string value, const std::vector<std::pair<std::string, std::string>>& replacements) {
    for (const auto& [key, replacement] : replacements) {
        std::size_t position = 0;
        while ((position = value.find(key, position)) != std::string::npos) {
            value.replace(position, key.size(), replacement);
            position += replacement.size();
        }
    }
    return value;
}
}

bool importLanguageProfiles(const std::string& path, std::string& error) {
    QFile input(QString::fromStdString(path));
    if (!input.open(QIODevice::ReadOnly) || input.size() > 4 * 1024 * 1024) {
        error = "Cannot read the language adapter manifest.";
        return false;
    }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(input.readAll(), &parse);
    const auto additions = document.object().value("languages").toArray();
    if (parse.error != QJsonParseError::NoError || additions.isEmpty()) {
        error = "Invalid language adapter manifest.";
        return false;
    }
    for (const auto& value : additions) {
        const auto profile = value.toObject();
        if (profile.value("key").toString().isEmpty() || profile.value("name").toString().isEmpty() ||
            profile.value("extensions").toArray().isEmpty()) {
            error = "A language adapter requires a key, name and file extensions.";
            return false;
        }
    }
    auto destination = qEnvironmentVariable("TS_LANGUAGE_PROFILES");
    if (destination.isEmpty())
        destination = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/languages.json";
    QJsonArray merged;
    QFile previous(destination);
    if (previous.open(QIODevice::ReadOnly) && previous.size() <= 4 * 1024 * 1024)
        merged = QJsonDocument::fromJson(previous.readAll()).object().value("languages").toArray();
    previous.close();
    for (const auto& value : additions) {
        for (int index = merged.size() - 1; index >= 0; --index)
            if (merged[index].toObject().value("key") == value.toObject().value("key"))
                merged.removeAt(index);
        merged.append(value);
    }
    QDir().mkpath(QFileInfo(destination).absolutePath());
    QSaveFile output(destination);
    const auto content = QJsonDocument(QJsonObject{{"version", 1}, {"languages", merged}}).toJson();
    if (!output.open(QIODevice::WriteOnly) || output.write(content) != content.size() || !output.commit()) {
        error = "Cannot save language adapters.";
        return false;
    }
    std::atomic_store(&storage(), std::shared_ptr<const Registry>(std::make_shared<Registry>(load())));
    return true;
}

LanguageInfo profileFor(LanguageId id) {
    const auto snapshot = registry();
    for (const auto& profile : snapshot->profiles)
        if (profile.id == id)
            return profile;
    return {};
}

const char* languageName(LanguageId id) {
    thread_local std::string name;
    name = profileFor(id).name;
    return name.empty() ? "Unknown" : name.c_str();
}

LanguageInfo languageForPath(const std::string& sourcePath) {
    const auto snapshot = registry();
    auto extension = pathToUtf8(pathFromUtf8(sourcePath).extension());
    if (!extension.empty())
        extension.erase(0, 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (const auto& profile : snapshot->profiles)
        for (const auto& suffix : profile.extensions)
            if (extension == suffix)
                return profile;
    return {};
}

Array<LanguageInfo> availableLanguages() {
    const auto snapshot = registry();
    Array<LanguageInfo> result;
    for (const auto& profile : snapshot->profiles)
        result.pushBack(profile);
    return result;
}

BuildPlan buildPlanFor(const std::string& sourcePath, const std::string& buildDirectory) {
    const auto snapshot = registry();
    BuildPlan plan;
    plan.language = languageForPath(sourcePath);
    if (!plan.language.runnable || sourcePath.empty())
        return plan;
    QJsonObject definition;
    for (std::size_t index = 0; index < snapshot->profiles.size(); ++index)
        if (snapshot->profiles[index].id == plan.language.id)
            definition = snapshot->definitions[index];
    const auto extension =
        QString::fromStdString(pathToUtf8(pathFromUtf8(sourcePath).extension())).mid(1).toLower();
    if (definition.value("excludedExtensions").toArray().contains(extension))
        return plan;
    const auto source = fs::absolute(pathFromUtf8(sourcePath)).lexically_normal();
    const auto build = fs::absolute(pathFromUtf8(buildDirectory)).lexically_normal();
    auto outputName = pathToUtf8(source.stem());
#ifdef _WIN32
    if (definition.value("compiled").toBool())
        outputName += ".exe";
#endif
    if (definition.contains("output"))
        outputName = definition.value("output").toString().toStdString();
    plan.outputPath = pathToUtf8(build / pathFromUtf8(outputName));
    const std::vector<std::pair<std::string, std::string>> replacements = {
        {"{file}", pathToUtf8(source)},
        {"{build}", pathToUtf8(build)},
        {"{output}", plan.outputPath},
        {"{stem}", pathToUtf8(source.stem())},
        {"{sourceDir}", pathToUtf8(source.parent_path())}};
    const auto command = [&](const QJsonObject& recipe) {
        ProcessSpec process;
        process.program = expand(recipe.value("program").toString().toStdString(), replacements);
        process.valid = !process.program.empty();
        process.workingDirectory = pathToUtf8(source.parent_path());
        for (const auto& argument : recipe.value("arguments").toArray())
            process.arguments.pushBack(expand(argument.toString().toStdString(), replacements));
        process.standardInputFile = expand(recipe.value("stdin").toString().toStdString(), replacements);
        return process;
    };
    plan.build = command(definition.value("build").toObject());
    plan.run = command(definition.value("run").toObject());
    plan.compiled = definition.value("compiled").toBool();
    plan.supported = plan.run.valid;
    return plan;
}

bool prepareSingleFileBuild(const BuildPlan& plan, const std::string& source, const std::string& build,
                            std::string& error) {
    if (plan.language.preparation != "dotnet")
        return true;
    std::error_code ec;
    std::filesystem::create_directories(pathFromUtf8(build), ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    std::string escaped;
    for (char value : source) {
        if (value == '&')
            escaped += "&amp;";
        else if (value == '<')
            escaped += "&lt;";
        else if (value == '>')
            escaped += "&gt;";
        else if (value == '\"')
            escaped += "&quot;";
        else if (value == '\n')
            escaped += "&#10;";
        else
            escaped += value;
    }
    const std::string xml =
        "<Project "
        "Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net8.0</"
        "TargetFramework><EnableDefaultCompileItems>false</EnableDefaultCompileItems><ImplicitUsings>enable</"
        "ImplicitUsings><Nullable>enable</Nullable><NuGetAudit>false</"
        "NuGetAudit><AssemblyName>VeyraUserProgram</AssemblyName></PropertyGroup><ItemGroup><Compile "
        "Include=\"" +
        escaped + "\" /></ItemGroup></Project>";
    return writeFileAtomically(pathToUtf8(pathFromUtf8(build) / "VeyraUserProgram.csproj"), xml, error) &&
           writeFileAtomically(pathToUtf8(pathFromUtf8(build) / "NuGet.Config"),
                               "<configuration><packageSources><clear /></packageSources></configuration>",
                               error);
}

}
