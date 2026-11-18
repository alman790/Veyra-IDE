#pragma once
#include "BackgroundJob.hpp"
#include <QList>
#include <QString>

struct ProjectMatch {
    QString path;
    int line = 1, column = 1;
    QString text;
};

struct ProjectSearchResult {
    QList<ProjectMatch> matches;
    int files = 0;
    bool truncated = false, cancelled = false;
    QString error;
};

class ProjectSearch final {
  public:
    static ProjectSearchResult scan(const QString& root, const QString& query, bool filenames,
                                    const BackgroundJob::Cancel& cancelled);
};
