#pragma once

#include "ts/LanguageTools.hpp"
#include <QJsonObject>
#include <QString>

struct NewProject {
    QString parent;
    QString name;
    ts::LanguageId language = ts::LanguageId::Cpp;
    QJsonObject sdk;
};

class ProjectCreator final {
  public:
    static bool create(const NewProject& project, QString& root, QString& entry, QString& error);
};
