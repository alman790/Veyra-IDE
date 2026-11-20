#include "ProjectRules.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

namespace {
QString glob(const QString& pattern) {
    QString result;
    for (int i = 0; i < pattern.size(); ++i) {
        const auto ch = pattern[i];
        if (ch == '\\' && i + 1 < pattern.size()) {
            result += QRegularExpression::escape(pattern.mid(++i, 1));
        } else if (ch == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                ++i;
                if (i + 1 < pattern.size() && pattern[i + 1] == '/') {
                    ++i;
                    result += "(?:.*/)?";
                } else {
                    result += ".*";
                }
            } else {
                result += "[^/]*";
            }
        } else if (ch == '?') {
            result += "[^/]";
        } else if (ch == '[') {
            const auto end = pattern.indexOf(']', i + 1);
            if (end > i + 1) {
                auto range = pattern.mid(i + 1, end - i - 1);
                if (range.startsWith('!'))
                    range[0] = '^';
                result += '[' + range + ']';
                i = end;
            } else {
                result += "\\[";
            }
        } else {
            result += QRegularExpression::escape(QString(ch));
        }
    }
    return result;
}
}

QList<ProjectRules::Rule> ProjectRules::read(const QString& path) {
    QFile file(path);
    QList<Rule> result;
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
        return result;
    for (auto line : QString::fromUtf8(file.readAll()).split('\n')) {
        if (line.endsWith('\r'))
            line.chop(1);
        while (line.endsWith(' ') && !line.endsWith("\\ "))
            line.chop(1);
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        Rule rule;
        rule.negate = line.startsWith('!');
        if (rule.negate)
            line.remove(0, 1);
        rule.directory = line.endsWith('/');
        if (rule.directory)
            line.chop(1);
        const bool anchored = line.contains('/');
        if (line.startsWith('/'))
            line.remove(0, 1);
        if (line.isEmpty())
            continue;
        rule.pattern = QRegularExpression((anchored ? "^" : "(?:^|/)") + glob(line) + '$');
        if (rule.pattern.isValid())
            result.append(rule);
    }
    return result;
}

ProjectRules::ProjectRules(const QString& root) : root_(QDir(root).absolutePath()) {
    QProcess git;
    git.setWorkingDirectory(root_);
    git.start("git", {"config", "--path", "core.excludesFile"});
    bool globalConfigured = false;
    if (git.waitForFinished(1000) && git.exitCode() == 0) {
        globalConfigured = true;
        const auto path = QString::fromUtf8(git.readAllStandardOutput()).trimmed();
        if (!path.isEmpty())
            base_ += read(QDir(root_).absoluteFilePath(path));
    } else if (git.state() != QProcess::NotRunning) {
        git.kill();
        git.waitForFinished(100);
    }
    const auto xdg = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config");
    if (!globalConfigured)
        base_ += read(xdg + "/git/ignore");
    QString gitDirectory = QDir(root_).filePath(".git");
    QFile marker(gitDirectory);
    if (QFileInfo(gitDirectory).isFile() && marker.open(QIODevice::ReadOnly)) {
        const auto line = QString::fromUtf8(marker.readLine()).trimmed();
        if (line.startsWith("gitdir: "))
            gitDirectory = QDir(root_).absoluteFilePath(line.mid(8));
    }
    QFile common(gitDirectory + "/commondir");
    if (common.open(QIODevice::ReadOnly))
        gitDirectory = QDir(gitDirectory).absoluteFilePath(QString::fromUtf8(common.readLine()).trimmed());
    base_ += read(gitDirectory + "/info/exclude");
    git.start("git", {"ls-files", "--cached", "-z"});
    if (git.waitForFinished(1000) && git.exitCode() == 0) {
        for (const auto& entry : git.readAllStandardOutput().split('\0')) {
            if (entry.isEmpty())
                continue;
            const auto path = QString::fromUtf8(entry);
            tracked_.insert(path);
            int slash = path.indexOf('/');
            while (slash >= 0) {
                trackedDirectories_.insert(path.left(slash));
                slash = path.indexOf('/', slash + 1);
            }
        }
    } else if (git.state() != QProcess::NotRunning) {
        git.kill();
        git.waitForFinished(100);
    }
}

const QList<ProjectRules::Rule>& ProjectRules::local(const QString& directory) const {
    const QFileInfo git(QDir(root_).filePath(directory.isEmpty() ? ".gitignore" : directory + "/.gitignore"));
    const QFileInfo custom(
        QDir(root_).filePath(directory.isEmpty() ? ".veyraignore" : directory + "/.veyraignore"));
    auto found = rules_.constFind(directory);
    if (found == rules_.cend() || found->gitTime != git.lastModified() ||
        found->customTime != custom.lastModified() || found->gitSize != git.size() ||
        found->customSize != custom.size()) {
        Cached cached{git.lastModified(), custom.lastModified(), git.size(), custom.size(),
                      read(git.filePath())};
        auto entries = read(custom.filePath());
        for (auto& rule : entries)
            rule.custom = true;
        cached.entries += entries;
        found = rules_.insert(directory, cached);
    }
    return found->entries;
}

bool ProjectRules::excludes(const QString& relative, bool directory) const {
    if (relative.isEmpty() || QDir::isAbsolutePath(relative))
        return true;
    const auto parts = QDir::fromNativeSeparators(relative).split('/');
    if (parts.contains("..") || parts.contains(".git") || parts.contains(".veyra"))
        return true;
    QList<QPair<QString, QList<Rule>>> levels{{QString(), base_}};
    const bool tracked = tracked_.contains(relative) || (directory && trackedDirectories_.contains(relative));
    QString parent;
    for (int i = 0; i < parts.size(); ++i) {
        levels.append({parent, local(parent)});
        const auto path = parts.mid(0, i + 1).join('/');
        const bool isDirectory = i < parts.size() - 1 || directory;
        bool excluded = false;
        for (const auto& level : levels) {
            const auto scoped = level.first.isEmpty() ? path : path.mid(level.first.size() + 1);
            for (const auto& rule : level.second)
                if ((!tracked || rule.custom) && (!rule.directory || isDirectory) &&
                    rule.pattern.match(scoped).hasMatch())
                    excluded = !rule.negate;
        }
        if (excluded)
            return true;
        parent = path;
    }
    return false;
}
