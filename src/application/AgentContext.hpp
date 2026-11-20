#pragma once

#include "BackgroundJob.hpp"
#include "ProjectRules.hpp"
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <memory>
#include <string>

struct AgentInput {
    QString root, path, text, language, sdk, buildOutput;
    int cursor = 0, selectionStart = 0, selectionEnd = 0;
    bool requestChanges = false;
    QMap<QString, QString> openFiles;
    QJsonArray diagnostics;
    QStringList configurationFiles;
    QStringList unavailableBuffers;
};

struct AgentEdit {
    QString path, before, after;
    int offset = -1;
};

struct AgentProposal {
    QString summary;
    QList<AgentEdit> edits;
};

class AgentContext final {
    AgentInput input_;
    std::shared_ptr<ProjectRules> rules_;
    QStringList files_;
    QMap<QString, QString> snapshots_;
    QStringList related_;
    QSet<QString> exposed_;
    QMap<QString, QList<int>> readPositions_;
    QString canonicalPath(const QString& relative) const;
    QString excerpt(const QString& path, const QString& text, int position, int limit) const;

  public:
    static AgentContext collect(const AgentInput& input, const BackgroundJob::Cancel& cancelled);
    static bool allowedPath(const QString& relative);
    QString prompt(int limit = 14000) const;
    QString read(const QString& path, int line = 1);
    QString search(const QString& query, const BackgroundJob::Cancel& cancelled);
    bool proposal(const QJsonObject& response, AgentProposal& result, QString& error) const;
    QString preview(const AgentProposal& proposal) const;
    std::string editGrammar() const;
    QList<AgentEdit> identifierRepairs() const;

    bool changesRequested() const {
        return input_.requestChanges;
    }

    const QMap<QString, QString>& snapshots() const {
        return snapshots_;
    }

    const QStringList& files() const {
        return files_;
    }

    QString root() const {
        return input_.root;
    }

    QString activePath() const {
        return input_.path;
    }
};
