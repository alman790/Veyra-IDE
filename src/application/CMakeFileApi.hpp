#pragma once
#include "ts/LanguageTools.hpp"
#include <QList>
#include <QString>

struct CMakeSettings {
    QString root, build, generator, configuration = "Debug", target;
};

struct CMakeTarget {
    QString name, type, artifact, configuration;
};

class CMakeFileApi final {
  public:
    static bool prepareQuery(const QString& build, QString& error);
    static QList<CMakeTarget> readTargets(const QString& build, const QString& configuration, QString& error);
    static ts::ProcessSpec configureCommand(const CMakeSettings& settings);
    static ts::ProcessSpec buildCommand(const CMakeSettings& settings, const QString& target);
    static ts::ProcessSpec runCommand(const CMakeSettings& settings, const CMakeTarget& target);
};
