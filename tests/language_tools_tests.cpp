#include "ts/LanguageTools.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* why) {
    if (!value)
        throw std::runtime_error(why);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    qputenv("TS_LANGUAGE_PROFILES", (temporary.path() + "/active.json").toUtf8());
    try {
        const auto before = ts::availableLanguages().size();
        const auto manifest = temporary.path() + "/adapter.json";
        QFile file(manifest);
        require(file.open(QIODevice::WriteOnly), "Create extension manifest");
        const QJsonObject adapter{
            {"key", "fixture-language"},
            {"name", "Fixture language"},
            {"extensions", QJsonArray{"fixture"}},
            {"runnable", true},
            {"entry", "main.fixture"},
            {"server", "fixture-language-server --stdio"},
            {"build", QJsonObject{{"program", "compiler with spaces"},
                                  {"arguments", QJsonArray{"{file}", "--output", "{output}"}}}},
            {"run", QJsonObject{{"program", "{output}"}}}};
        file.write(QJsonDocument(QJsonObject{{"languages", QJsonArray{adapter}}}).toJson());
        file.close();
        std::string error;
        require(ts::importLanguageProfiles(manifest.toStdString(), error), error.c_str());
        require(ts::availableLanguages().size() == before + 1,
                "New language loads without a C++ case or recompilation");
        const auto profile = ts::languageForPath("source.FIXTURE");
        require(profile.lspId == "fixture-language" &&
                    profile.suggestedServer == "fixture-language-server --stdio",
                "File type and LSP come from manifest");
        const auto plan = ts::buildPlanFor((temporary.path() + "/file with spaces.fixture").toStdString(),
                                           (temporary.path() + "/build folder").toStdString());
        require(plan.supported && plan.build.program == "compiler with spaces",
                "Build command comes from manifest");
        require(plan.build.arguments[0].find("file with spaces.fixture") != std::string::npos &&
                    plan.build.arguments[2].find("build folder") != std::string::npos,
                "Placeholder substitution preserves single arguments");
        require(ts::languageForPath("unknown.extension").id == ts::LanguageId::Unknown,
                "Unknown files remain editable plain text");
        require(ts::languageForPath("file.CPP").lspId == "cpp", "Built-in adapter remains available");
        std::cout
            << "PASS: declarative language adapters, runtime import, LSP and argument-safe build plans\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
