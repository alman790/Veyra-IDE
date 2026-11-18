#pragma once
#include "ts/LanguageTools.hpp"
#include <QString>

inline bool prepareSingleFileProject(const ts::BuildPlan& plan, const QString& source, const QString& build,
                                     QString& error) {
    std::string issue;
    const bool success = ts::prepareSingleFileBuild(plan, source.toStdString(), build.toStdString(), issue);
    error = QString::fromStdString(issue);
    return success;
}
