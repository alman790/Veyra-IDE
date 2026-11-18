#include "ProjectCreator.hpp"
#include "../gui/SourceTemplates.hpp"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>

bool ProjectCreator::create(const NewProject& project, QString& root, QString& entry, QString& error) {
    root.clear();
    entry.clear();
    error.clear();
    if (!QRegularExpression("^[\\p{L}\\p{N}_][\\p{L}\\p{N}_ .-]*$").match(project.name).hasMatch() ||
        project.name.endsWith('.') || project.name.endsWith(' ')) {
        error = "Enter a project name without path separators.";
        return false;
    }
    const auto sdkRoot = project.sdk.value("root").toString();
    if (!sdkRoot.isEmpty() && !QDir(sdkRoot).exists()) {
        error = "SDK directory is unavailable.";
        return false;
    }
    const QDir parent(project.parent);
    if (!parent.exists()) {
        error = "Parent directory is unavailable.";
        return false;
    }
    const auto destination = parent.absoluteFilePath(project.name);
    if (QFileInfo::exists(destination)) {
        error = "Project path already exists.";
        return false;
    }
    QTemporaryDir staging(parent.absoluteFilePath(".veyra-new-XXXXXX"));
    if (!staging.isValid()) {
        error = "Could not create the project directory.";
        return false;
    }
    const QDir directory(staging.path());
    const auto write = [&](const QString& relative, const QByteArray& content) {
        const auto path = directory.filePath(relative);
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
            error = "Could not create directory: " + relative;
            return false;
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit()) {
            error = "Could not write " + relative + ": " + file.errorString();
            return false;
        }
        return true;
    };
    const auto language = ts::profileFor(project.language);
    const auto relativeEntry = QString::fromStdString(language.entry);
    if (relativeEntry.isEmpty() || QDir::isAbsolutePath(relativeEntry) ||
        relativeEntry.split('/').contains("..")) {
        error = "This language profile has no valid project entry.";
        return false;
    }
    if (!write(relativeEntry, sourceTemplate(project.language, QFileInfo(relativeEntry).fileName()).toUtf8()))
        return false;
    const QJsonObject metadata{{"version", 1},
                               {"language", QString::fromStdString(language.lspId)},
                               {"sdk", project.sdk},
                               {"entry", relativeEntry}};
    if (!write(".veyra/project.json", QJsonDocument(metadata).toJson()) ||
        !write(".gitignore", ".veyra/build/\n.veyra/cmake-*/\n__pycache__/\nnode_modules/\n"))
        return false;
    for (const auto& [name, content] : language.projectFiles) {
        const auto relative = QString::fromStdString(name);
        if (QDir::isAbsolutePath(relative) || relative.split('/').contains("..") || relative.contains('\\')) {
            error = "Invalid project template path.";
            return false;
        }
        if (!write(relative, QByteArray::fromStdString(content)))
            return false;
    }
    if (!QDir().rename(staging.path(), destination)) {
        error = "Could not finish creating the project.";
        return false;
    }
    staging.setAutoRemove(false);
    root = QDir(destination).canonicalPath();
    entry = QDir(root).filePath(relativeEntry);
    return true;
}
