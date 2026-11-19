#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>
#include <algorithm>

namespace {
QString read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
}

QString decode(const QString& literals, const QRegularExpression& quoted) {
    QString result;
    auto parts = quoted.globalMatch(literals);
    while (parts.hasNext()) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(('[' + parts.next().captured() + ']').toUtf8(), &error);
        if (error.error != QJsonParseError::NoError)
            return {};
        result += document.array().first().toString();
    }
    return result;
}

QStringList placeholders(const QString& value) {
    QStringList result;
    const QRegularExpression expression("%[1-9]");
    auto matches = expression.globalMatch(value);
    while (matches.hasNext())
        result << matches.next().captured();
    std::sort(result.begin(), result.end());
    return result;
}

int translations(const QString& root) {
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(read(root + "/resources/i18n/en.json").toUtf8(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        QTextStream(stderr) << "Cannot read translation catalog\n";
        return 1;
    }
    const auto catalog = document.object();
    const QRegularExpression cyrillic(QString::fromUtf8("[А-Яа-яЁё]"));
    const QString literal = R"REGEX("(?:[^"\\]|\\.)*")REGEX";
    const QString cluster = literal + "(?:\\s*" + literal + ")*";
    const QRegularExpression quoted(literal), strings(cluster);
    const QRegularExpression calls("UiLanguage::(?:tr|text)\\((" + cluster + ')');
    QSet<QString> english;
    QStringList errors;
    for (auto it = catalog.begin(); it != catalog.end(); ++it) {
        const auto value = it.value().toString();
        english.insert(value);
        if (value.isEmpty() || cyrillic.match(value).hasMatch())
            errors << "Invalid English translation: " + it.key();
        if (placeholders(value) != placeholders(it.key()))
            errors << "Translation placeholders differ: " + it.key();
    }
    const QHash<QString, QSet<QString>> exceptions{
        {"UiLanguage.cpp", {QString::fromUtf8("[А-Яа-яЁё]")}},
        {"WorkspaceWidgets.hpp",
         {QString::fromUtf8("ожидание"), QString::fromUtf8("Загрузка"), QString::fromUtf8("дедлайн"),
          QString::fromUtf8("Выполнено"), QString::fromUtf8("выполнены")}}};
    for (const auto& directory : {"src/gui", "src/core", "src/application", "include/ts"}) {
        QDirIterator files(root + '/' + directory, {"*.cpp", "*.hpp"}, QDir::Files);
        while (files.hasNext()) {
            const auto path = files.next();
            const auto source = read(path);
            auto matches = calls.globalMatch(source);
            while (matches.hasNext()) {
                const auto value = decode(matches.next().captured(1), quoted);
                if (!english.contains(value) && value != "SDK")
                    errors << QDir(root).relativeFilePath(path) + ": missing Russian translation: " + value;
            }
            matches = strings.globalMatch(source);
            while (matches.hasNext()) {
                const auto value = decode(matches.next().captured(), quoted);
                if (cyrillic.match(value).hasMatch() &&
                    !exceptions.value(QFileInfo(path).fileName()).contains(value) && !catalog.contains(value))
                    errors << QDir(root).relativeFilePath(path) + ": missing English translation: " + value;
            }
        }
    }
    if (!errors.isEmpty()) {
        QTextStream(stderr) << errors.join('\n') << '\n';
        return 1;
    }
    QTextStream(stdout) << "PASS: translation coverage (" << catalog.size() << " entries)\n";
    return 0;
}

int smoke(const QString& executable, const QString& platform) {
    if (!QFileInfo::exists(executable)) {
        QTextStream(stderr) << "Application not found: " << executable << '\n';
        return 1;
    }
    QTemporaryDir directory;
    if (!directory.isValid())
        return 1;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("XDG_CONFIG_HOME", directory.path());
    environment.remove("QT_PLUGIN_PATH");
    environment.remove("QT_QPA_PLATFORM_PLUGIN_PATH");
    if (!platform.isEmpty())
        environment.insert("QT_QPA_PLATFORM", platform);
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(directory.path());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QFileInfo(executable).absoluteFilePath(), {});
    if (!process.waitForStarted(10000)) {
        QTextStream(stderr) << process.errorString() << '\n';
        return 1;
    }
    if (process.waitForFinished(5000)) {
        QTextStream(stderr) << "GUI exited unexpectedly (" << process.exitCode() << "):\n"
                            << process.readAll() << '\n';
        return 1;
    }
    process.terminate();
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(2000);
    }
    QTextStream(stdout) << "PASS: GUI remains running: " << executable << '\n';
    return 0;
}
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.size() == 3 && arguments[1] == "translations")
        return translations(arguments[2]);
    if (arguments.size() >= 3 && arguments.size() <= 4 && arguments[1] == "smoke")
        return smoke(arguments[2], arguments.value(3));
    QTextStream(stderr) << "Usage: veyra_devtools translations <source-root>\n"
                           "       veyra_devtools smoke <executable> [platform]\n";
    return 2;
}
