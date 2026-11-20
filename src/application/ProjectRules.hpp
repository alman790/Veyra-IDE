#pragma once
#include <QDateTime>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QString>

class ProjectRules final {
    struct Rule {
        QRegularExpression pattern;
        bool negate = false;
        bool directory = false;
        bool custom = false;
    };

    QString root_;

    struct Cached {
        QDateTime gitTime, customTime;
        qint64 gitSize = -1, customSize = -1;
        QList<Rule> entries;
    };

    mutable QHash<QString, Cached> rules_;
    QList<Rule> base_;
    QSet<QString> tracked_, trackedDirectories_;
    static QList<Rule> read(const QString& path);
    const QList<Rule>& local(const QString& directory) const;

  public:
    explicit ProjectRules(const QString& root = {});
    bool excludes(const QString& relative, bool directory = false) const;
};
