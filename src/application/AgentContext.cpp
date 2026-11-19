#include "AgentContext.hpp"
#include "DocumentController.hpp"
#include "ts/LanguageTools.hpp"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <numeric>
#include <stdexcept>

bool AgentContext::allowedPath(const QString& relative) {
    if (relative.isEmpty() || QDir::isAbsolutePath(relative) || relative.contains('\\'))
        return false;
    static const QSet<QString> excluded{".beamcore",   ".config",     ".gnupg", ".kube",        ".docker",
                                        ".git",        ".hg",         ".svn",   ".idea",        ".veyra",
                                        ".ssh",        ".aws",        ".azure", "node_modules", "vendor",
                                        "third_party", "deps",        "_build", "build",        "dist",
                                        "target",      "__pycache__", ".venv",  "venv",         ".cache"};
    const auto parts = relative.split('/');
    for (const auto& part : parts)
        if (part.isEmpty() || part == "." || part == ".." || excluded.contains(part.toLower()) ||
            part.startsWith("build-") || part.startsWith("cmake-build") || part.endsWith(".noindex"))
            return false;
    const auto name = parts.last().toLower();
    if (name == ".erlang.cookie" || name == ".netrc" || name == ".npmrc" || name == ".pypirc" ||
        name == ".git-credentials" || name == ".env" || name.startsWith(".env.") ||
        name.contains("credentials") || name.contains("secrets") || name == "id_rsa" ||
        name == "id_ed25519" || name.endsWith(".pem") || name.endsWith(".key") || name.endsWith(".p12") ||
        name.endsWith(".pfx") || name.endsWith(".gguf") || name.endsWith(".dets"))
        return false;
    return true;
}

QString AgentContext::canonicalPath(const QString& relative) const {
    if (!allowedPath(relative))
        return {};
    const auto canonical = QFileInfo(QDir(input_.root).filePath(relative)).canonicalFilePath();
    if (canonical.isEmpty() || !canonical.startsWith(input_.root + '/') || !QFileInfo(canonical).isFile())
        return {};
    return canonical;
}

QString AgentContext::excerpt(const QString& path, const QString& text, int position, int limit) const {
    int start = std::max(0, std::min(position, int(text.size())) - limit / 2);
    if (start > 0 && text[start - 1] != '\n') {
        const int newline = text.indexOf('\n', start);
        if (newline >= 0 && newline < position)
            start = newline + 1;
    }
    const int line = text.left(start).count('\n') + 1;
    return QString("File: %1 (from line %2)\n%3").arg(path).arg(line).arg(text.mid(start, limit));
}

AgentContext AgentContext::collect(const AgentInput& input, const BackgroundJob::Cancel& cancelled) {
    AgentContext result;
    result.input_ = input;
    result.input_.root = QDir(input.root).canonicalPath();
    const auto active = QFileInfo(input.path).canonicalFilePath();
    if (result.input_.root.isEmpty() || active.isEmpty() || !active.startsWith(result.input_.root + '/'))
        throw std::runtime_error("Open a saved file inside the project before starting the agent.");
    result.input_.path = QDir(result.input_.root).relativeFilePath(active);
    if (!allowedPath(result.input_.path))
        throw std::runtime_error("This file is excluded from agent context.");
    result.snapshots_[result.input_.path] = input.text;
    result.exposed_.insert(result.input_.path);
    for (auto it = input.openFiles.begin(); it != input.openFiles.end(); ++it) {
        const auto relative =
            QDir(result.input_.root).relativeFilePath(QFileInfo(it.key()).canonicalFilePath());
        if (relative == result.input_.path)
            continue;
        if (allowedPath(relative) && !result.canonicalPath(relative).isEmpty())
            result.snapshots_[relative] = it.value();
    }
    QStringList directories{result.input_.root};
    int inspected = 0;
    while (!directories.isEmpty() && inspected < 12000 && result.files_.size() < 3000 && !cancelled()) {
        const auto directory = directories.takeLast();
        QDirIterator entries(directory, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
        while (entries.hasNext()) {
            if (cancelled() || ++inspected > 12000)
                break;
            entries.next();
            const auto entry = entries.fileInfo();
            const auto relative = QDir(result.input_.root).relativeFilePath(entry.filePath());
            if (!allowedPath(relative) || entry.isSymLink())
                continue;
            if (entry.isDir()) {
                if (relative.count('/') < 12)
                    directories.append(entry.filePath());
            } else if (entry.size() <= 1024 * 1024 && result.files_.size() < 3000)
                result.files_.append(relative);
        }
    }
    result.files_.sort();
    const auto stem = QFileInfo(result.input_.path).completeBaseName();
    const auto nearby = input.text.mid(std::max(0, input.cursor - 2000), 4000);
    QList<QPair<int, QString>> ranked;
    for (const auto& path : result.files_) {
        if (path == result.input_.path)
            continue;
        const auto info = QFileInfo(path);
        int score = nearby.contains(info.fileName()) ? 100 : 0;
        score += info.completeBaseName() == stem ? 80 : 0;
        score += nearby.contains(
                     QRegularExpression("\\b" + QRegularExpression::escape(info.completeBaseName()) + "\\b"))
                     ? 40
                     : 0;
        score += input.openFiles.contains(QDir(result.input_.root).filePath(path)) ? 20 : 0;
        score += input.configurationFiles.contains(path) ? 30 : 0;
        if (score > 0)
            ranked.append({score, path});
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& candidate : ranked) {
        if (result.related_.size() >= 4 || cancelled())
            break;
        const auto content = result.read(candidate.second);
        if (!content.startsWith("Error:"))
            result.related_.append(candidate.second);
    }
    return result;
}

QString AgentContext::prompt(int limit) const {
    QString context = QString("Project: %1\nActive file: %2\nLanguage: %3\nSDK: %4\nCursor UTF-16 offset: "
                              "%5\nSelection: %6..%7\n")
                          .arg(QStringLiteral("."), input_.path, input_.language, input_.sdk)
                          .arg(input_.cursor)
                          .arg(input_.selectionStart)
                          .arg(input_.selectionEnd);
    context += "\nDiagnostics (file, line, message):\n" +
               QString::fromUtf8(QJsonDocument(input_.diagnostics).toJson(QJsonDocument::Compact)).left(1500);
    context +=
        "\nMost recent compiler/build output (may precede unsaved edits):\n" + input_.buildOutput.right(1200);
    if (input_.selectionEnd > input_.selectionStart)
        context += "\nSelected code:\n" +
                   input_.text.mid(input_.selectionStart,
                                   std::min(2000, input_.selectionEnd - input_.selectionStart));
    context += "\n\n" + excerpt(input_.path, input_.text, input_.cursor, 5000);
    for (const auto& path : related_)
        context += "\n\n" + excerpt(path, snapshots_.value(path), 0, 1200);
    context += "\n\nProject files (bounded listing):\n" + files_.mid(0, 180).join('\n').left(1800);
    return context.left(limit);
}

QString AgentContext::read(const QString& path, int line) {
    if (input_.unavailableBuffers.contains(path))
        return "Error: this open buffer exceeds the context snapshot limit. Make it the active file and "
               "submit a new request.";
    const auto absolute = canonicalPath(path);
    if (absolute.isEmpty() || QDir(input_.root).relativeFilePath(absolute) != path)
        return "Error: file is outside the project or excluded from context.";
    if (!snapshots_.contains(path)) {
        if (snapshots_.size() >= 16 || QFileInfo(absolute).size() > 1024 * 1024)
            return "Error: context file limit exceeded.";
        QString text, error;
        if (!DocumentController::readUtf8(absolute, text, error))
            return "Error: " + error;
        snapshots_[path] = text;
    }
    const auto& text = snapshots_[path];
    exposed_.insert(path);
    int position = 0;
    for (int current = 1; current < std::clamp(line, 1, 1000000); ++current) {
        const auto next = text.indexOf('\n', position);
        if (next < 0)
            break;
        position = next + 1;
    }
    auto& positions = readPositions_[path];
    if (!positions.contains(position))
        positions.prepend(position);
    if (positions.size() > 8)
        positions.removeLast();
    return QString("File: %1 (from line %2)\n%3")
        .arg(path)
        .arg(text.left(position).count('\n') + 1)
        .arg(text.mid(position, 5000));
}

QString AgentContext::search(const QString& query, const BackgroundJob::Cancel& cancelled) {
    if (query.size() < 2 || query.size() > 100)
        return "Error: search must contain 2 to 100 characters.";
    QStringList matches;
    int inspected = 0;
    for (const auto& path : files_) {
        if (cancelled() || ++inspected > 300 || matches.size() >= 20)
            break;
        QString text, error;
        if (snapshots_.contains(path))
            text = snapshots_.value(path);
        else {
            const auto absolute = canonicalPath(path);
            if (absolute.isEmpty() || QFileInfo(absolute).size() > 128 * 1024 ||
                !DocumentController::readUtf8(absolute, text, error))
                continue;
        }
        const int position = text.indexOf(query, 0, Qt::CaseSensitive);
        if (position >= 0)
            matches << QString("%1:%2: %3")
                           .arg(path)
                           .arg(text.left(position).count('\n') + 1)
                           .arg(text.mid(std::max(0, position - 50), 180).replace('\n', ' '));
    }
    return matches.isEmpty() ? "No matches in the bounded project search." : matches.join('\n');
}

bool AgentContext::proposal(const QJsonObject& response, AgentProposal& result, QString& error) const {
    result = {};
    result.summary = response.value("summary").toString();
    const auto edits = response.value("edits").toArray();
    if (result.summary.isEmpty() || edits.isEmpty() || edits.size() > 8) {
        error = "The agent returned an invalid proposal.";
        return false;
    }
    for (const auto& value : edits) {
        const auto object = value.toObject();
        AgentEdit edit{object.value("path").toString(), object.value("before").toString(),
                       object.value("after").toString()};
        const auto canonical = canonicalPath(edit.path);
        if (!allowedPath(edit.path) || !snapshots_.contains(edit.path) || !exposed_.contains(edit.path) ||
            canonical.isEmpty() || QDir(input_.root).relativeFilePath(canonical) != edit.path ||
            edit.before.isEmpty() || edit.before == edit.after || edit.before.size() > 16000 ||
            edit.after.size() > 16000 || edit.after.contains(QChar(0))) {
            error = "The proposal refers to unread content or an invalid replacement.";
            return false;
        }
        const auto& source = snapshots_[edit.path];
        edit.offset = source.indexOf(edit.before);
        if (edit.offset < 0 || source.indexOf(edit.before, edit.offset + 1) >= 0) {
            error = "A proposed replacement does not match one unique source location.";
            return false;
        }
        for (const auto& previous : result.edits)
            if (edit.path == previous.path && edit.offset < previous.offset + previous.before.size() &&
                previous.offset < edit.offset + edit.before.size()) {
                error = "The proposal contains overlapping replacements.";
                return false;
            }
        result.edits.append(edit);
    }
    error.clear();
    return true;
}

QString AgentContext::preview(const AgentProposal& proposal) const {
    QString result;
    for (const auto& edit : proposal.edits) {
        const int line = snapshots_.value(edit.path).left(edit.offset).count('\n') + 1;
        result += QString("--- %1\n+++ %1\n@@ line %2 @@\n").arg(edit.path).arg(line);
        for (const auto& text : edit.before.split('\n'))
            result += '-' + text + '\n';
        for (const auto& text : edit.after.split('\n'))
            result += '+' + text + '\n';
        result += '\n';
    }
    return result;
}

std::string AgentContext::editGrammar() const {
    const auto quote = [](const QString& value) {
        const auto encoded = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
        return QString::fromUtf8(encoded.mid(1, encoded.size() - 2));
    };
    const auto jsonLiteral = [&](const QString& value) { return quote(quote(value)); };
    const auto field = [&](const QString& name, const QString& pattern) {
        return jsonLiteral(name) + " ws \":\" ws " + pattern;
    };
    auto paths = exposed_.values();
    paths.sort();
    paths.removeOne(input_.path);
    paths.prepend(input_.path);
    QStringList alternatives;
    int grammarSize = 0;
    for (const auto& path : paths) {
        const auto& text = snapshots_[path];
        QStringList candidates;
        const auto add = [&](const QString& source) {
            if (source.trimmed().isEmpty() || source.size() > 3000 || candidates.contains(source))
                return;
            const int offset = text.indexOf(source);
            if (offset >= 0 && text.indexOf(source, offset + 1) < 0 && candidates.size() < 12)
                candidates.append(source);
        };
        if (path == input_.path && input_.selectionEnd > input_.selectionStart)
            add(text.mid(input_.selectionStart, input_.selectionEnd - input_.selectionStart));
        auto positions = readPositions_.value(path);
        if (path == input_.path && !positions.contains(input_.cursor))
            positions.append(std::clamp(input_.cursor, 0, int(text.size())));
        if (positions.isEmpty())
            positions.append(0);
        for (const int position : positions) {
            const int start = position == 0 ? 0 : text.lastIndexOf('\n', position - 1) + 1;
            int end = text.indexOf('\n', position);
            if (end < 0)
                end = text.size();
            add(text.mid(start, end - start));
        }
        for (const int position : positions)
            for (const auto& line : text.mid(std::max(0, position - 1200), 3000).split('\n'))
                add(line);
        if (text.size() <= 3000)
            add(text);
        QStringList literals;
        for (const auto& candidate : candidates) {
            const auto literal = jsonLiteral(candidate);
            if (grammarSize + literal.size() > 32000)
                break;
            grammarSize += literal.size();
            literals.append(literal);
        }
        if (literals.isEmpty())
            continue;
        alternatives << "\"{\" ws " + field("path", jsonLiteral(path)) + " ws \",\" ws " +
                            field("before", '(' + literals.join(" | ") + ')') + " ws \",\" ws " +
                            field("after", "string") + " ws \"}\"";
    }
    return alternatives.isEmpty() ? std::string() : ("edit ::= " + alternatives.join(" | ")).toStdString();
}

QList<AgentEdit> AgentContext::identifierRepairs() const {
    QString diagnostics = input_.buildOutput;
    for (const auto& item : input_.diagnostics)
        diagnostics += '\n' + item.toObject().value("message").toString();
    if (diagnostics.isEmpty())
        return {};
    const auto& source = snapshots_[input_.path];
    static const QRegularExpression identifier(R"([\p{L}_][\p{L}\p{M}\p{N}_]*)");
    static const QRegularExpression quoted(R"(["'`]([\p{L}_][\p{L}\p{M}\p{N}_]{1,63})["'`])");
    QStringList targets;
    auto tokens = identifier.globalMatch(source.mid(std::max(0, input_.cursor - 80), 160));
    while (tokens.hasNext()) {
        const auto word = tokens.next().captured();
        if (diagnostics.contains(word) && !targets.contains(word))
            targets.append(word);
    }
    auto quotes = quoted.globalMatch(diagnostics);
    while (quotes.hasNext()) {
        const auto word = quotes.next().captured(1);
        if (!targets.contains(word))
            targets.append(word);
    }
    QSet<QString> keywords, names;
    for (const auto& word : ts::languageForPath(input_.path.toStdString()).keywords)
        keywords.insert(QString::fromStdString(word));
    for (const auto& path : exposed_) {
        const auto& snapshot = snapshots_[path];
        auto positions = readPositions_.value(path);
        if (path == input_.path)
            positions.append(input_.cursor);
        if (positions.isEmpty())
            positions.append(0);
        for (const int position : positions) {
            auto words = identifier.globalMatch(snapshot.mid(std::max(0, position - 5000), 10000));
            while (words.hasNext() && names.size() < 5000) {
                const auto word = words.next().captured();
                if (word.size() >= 2 && word.size() <= 64 && !keywords.contains(word))
                    names.insert(word);
            }
        }
    }
    const auto distance = [](const QString& left, const QString& right) {
        QVector<int> previous(right.size() + 1), current(right.size() + 1);
        std::iota(previous.begin(), previous.end(), 0);
        for (int i = 1; i <= left.size(); ++i) {
            current[0] = i;
            for (int j = 1; j <= right.size(); ++j)
                current[j] = std::min({previous[j] + 1, current[j - 1] + 1,
                                       previous[j - 1] + (left[i - 1] == right[j - 1] ? 0 : 1)});
            previous.swap(current);
        }
        return previous.last();
    };
    QList<QPair<int, AgentEdit>> ranked;
    for (const auto& target : targets.mid(0, 16)) {
        if (target.size() < 3 || target.size() > 64 || keywords.contains(target))
            continue;
        const int position = source.indexOf(target);
        if (position < 0 || source.indexOf(target, position + 1) >= 0)
            continue;
        const auto part = [](QChar value) {
            return value.isLetterOrNumber() || value.isMark() || value == '_';
        };
        if ((position > 0 && part(source[position - 1])) ||
            (position + target.size() < source.size() && part(source[position + target.size()])))
            continue;
        bool declaredElsewhere = false;
        for (const auto& path : exposed_)
            if (path != input_.path && snapshots_[path].contains(QRegularExpression(
                                           "\\b" + QRegularExpression::escape(target) + "\\b")))
                declaredElsewhere = true;
        if (declaredElsewhere)
            continue;
        const int start = position == 0 ? 0 : source.lastIndexOf('\n', position - 1) + 1;
        const auto newline = source.indexOf('\n', position);
        const int end = newline < 0 ? int(source.size()) : int(newline);
        const auto before = source.mid(start, end - start);
        if (before.isEmpty() || before.size() > 3000)
            continue;
        auto orderedNames = names.values();
        orderedNames.sort();
        for (const auto& name : orderedNames) {
            if (name == target || std::abs(name.size() - target.size()) > 2)
                continue;
            const int score = distance(target, name);
            if (score < 1 || score > 2)
                continue;
            auto after = before;
            after.replace(position - start, target.size(), name);
            ranked.append({score, {input_.path, before, after, start}});
            if (ranked.size() > 64) {
                std::stable_sort(ranked.begin(), ranked.end(),
                                 [](const auto& a, const auto& b) { return a.first < b.first; });
                ranked.resize(64);
            }
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    QList<AgentEdit> result;
    for (const auto& candidate : ranked) {
        if (result.size() >= 8)
            break;
        bool duplicate = false;
        for (const auto& previous : result)
            duplicate = duplicate || (previous.before == candidate.second.before &&
                                      previous.after == candidate.second.after);
        if (!duplicate)
            result.append(candidate.second);
    }
    return result;
}
