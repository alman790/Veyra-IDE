#pragma once
#include "ts/LanguageTools.hpp"
#include <QFileInfo>
#include <QString>

inline QString sourceExtension(ts::LanguageId id) {
    const auto profile = ts::profileFor(id);
    return profile.extensions.empty() ? QString("txt") : QString::fromStdString(profile.extensions[0]);
}

inline QString sourceTemplate(ts::LanguageId id, const QString& name) {
    return QString::fromStdString(ts::profileFor(id).sourceTemplate)
        .replace("{stem}", QFileInfo(name).completeBaseName());
}
