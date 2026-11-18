#pragma once
#include "Structures.hpp"
#include <string>

namespace ts {

enum class LanguageId {
    Unknown,
    C,
    Cpp,
    Python,
    Java,
    JavaScript,
    TypeScript,
    Rust,
    Go,
    Ruby,
    PHP,
    Lua,
    Bash,
    Swift,
    Kotlin,
    CSharp,
    Json,
    Html,
    Css,
    Sql,
    Markdown,
    PlainText
};

struct LanguageInfo {
    LanguageId id = LanguageId::Unknown;
    std::string name;
    std::string lspId;
    std::string suggestedServer;
    bool runnable = false;
    Array<std::string> extensions;
    Array<std::string> keywords;
    Array<std::string> compilerCandidates;
    Array<std::string> sdkExecutables;
    Array<std::string> sdkRoots;
    Array<std::string> sdkRequiredFiles;
    Array<std::string> quotePrefixes;
    Array<std::pair<std::string, std::string>> projectFiles;
    std::string sourceTemplate;
    std::string entry;
    std::string sdkVariable;
    std::string preparation;
    std::string lineComment;
    bool blockComments = false;
    std::string cmakeLanguage;
    std::string installation;
    std::string serverSettings;
    std::string initializationOptions;
    Array<std::pair<std::string, std::string>> highlightPatterns;
};

struct ProcessSpec {
    std::string program;
    Array<std::string> arguments;
    std::string workingDirectory;
    bool valid = false;
    std::string standardInputFile;
};

struct BuildPlan {
    LanguageInfo language;
    bool supported = false;
    bool compiled = false;
    ProcessSpec build;
    ProcessSpec run;
    std::string outputPath;
    std::string explanation;
};

bool importLanguageProfiles(const std::string& path, std::string& error);
LanguageInfo profileFor(LanguageId id);
LanguageInfo languageForPath(const std::string& sourcePath);
Array<LanguageInfo> availableLanguages();
BuildPlan buildPlanFor(const std::string& sourcePath, const std::string& buildDirectory);
bool prepareSingleFileBuild(const BuildPlan& plan, const std::string& source, const std::string& build,
                            std::string& error);
const char* languageName(LanguageId id);
}
