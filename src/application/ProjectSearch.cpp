#include "ProjectSearch.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringDecoder>

ProjectSearchResult ProjectSearch::scan(const QString& root, const QString& query, bool filenames,
                                        const BackgroundJob::Cancel& cancelled) {
    ProjectSearchResult result;
    if (query.isEmpty() || !QFileInfo(root).isDir()) {
        result.error = "Specify a project directory and a nonempty query.";
        return result;
    }
    QList<QString> pending{QDir(root).absolutePath()};
    qint64 bytes = 0;
    const QStringList ignored = {".git", ".veyra", ".idea", "node_modules", "target",
                                 "dist", "venv",   ".venv", "__pycache__"};
    while (!pending.isEmpty()) {
        if (cancelled && cancelled()) {
            result.cancelled = true;
            return result;
        }
        const auto directory = pending.takeLast();
        const auto entries = QDir(directory).entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
        for (const auto& entry : entries) {
            if (cancelled && cancelled()) {
                result.cancelled = true;
                return result;
            }
            if (entry.isDir()) {
                if (!ignored.contains(entry.fileName()) && !entry.fileName().startsWith("build") &&
                    !entry.fileName().startsWith("cmake-build"))
                    pending.append(entry.absoluteFilePath());
                continue;
            }
            if (++result.files > 6000 || result.matches.size() >= 2000) {
                result.truncated = true;
                return result;
            }
            if (filenames) {
                if (entry.fileName().contains(query, Qt::CaseInsensitive))
                    result.matches.append({entry.absoluteFilePath(), 1, 1, entry.fileName()});
                continue;
            }
            if (entry.size() > 2 * 1024 * 1024)
                continue;
            if (bytes + entry.size() > 128 * 1024 * 1024) {
                result.truncated = true;
                return result;
            }
            bytes += entry.size();
            QFile file(entry.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly))
                continue;
            QStringDecoder decoder(QStringDecoder::Utf8);
            const QString text = decoder(file.readAll());
            if (decoder.hasError() || text.contains(QChar(0)))
                continue;
            int line = 1, begin = 0;
            while (begin <= text.size()) {
                if (cancelled && cancelled()) {
                    result.cancelled = true;
                    return result;
                }
                const int newline = text.indexOf('\n', begin);
                const int end = newline < 0 ? text.size() : newline;
                const auto content = text.mid(begin, end - begin);
                int from = 0;
                while ((from = content.indexOf(query, from, Qt::CaseInsensitive)) >= 0) {
                    result.matches.append(
                        {entry.absoluteFilePath(), line, from + 1, content.left(240).trimmed()});
                    from += query.size();
                    if (result.matches.size() >= 2000) {
                        result.truncated = true;
                        return result;
                    }
                }
                if (newline < 0)
                    break;
                begin = end + 1;
                ++line;
            }
        }
    }
    return result;
}
