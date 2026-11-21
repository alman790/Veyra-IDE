#include "RemoteSession.hpp"
#include "UiLanguage.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}

static bool waitFor(const std::function<bool()>& ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 12000)
        QTest::qWait(10);
    return ready();
}

static void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Write remote fixture");
}

static QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Read remote fixture");
    return file.readAll();
}

int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    std::cout << "SKIP: the POSIX protocol fixture requires a POSIX host\n";
    return 77;
#endif
    QCoreApplication app(argc, argv);
    app.setOrganizationName("VeyraTests");
    app.setApplicationName("RemoteWorkspace");
    if (QStandardPaths::findExecutable("sh").isEmpty()) {
        std::cout << "SKIP: POSIX shell fixture is unavailable\n";
        return 77;
    }
    QTemporaryDir project, external, settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    try {
        require(argc == 2, "SSH fixture executable is required");
        const auto filename = QString::fromUtf8("source 'quoted' привет.cpp");
        write(project.filePath(filename), "int value = 1;\n");
        write(external.filePath("outside.cpp"), "private outside project\n");
        require(QFile::link(external.path(), project.filePath("outside-dir")),
                "Create external directory symlink");
        RemoteConnection connection{"fixture",
                                    {},
                                    project.path(),
                                    {},
                                    settings.filePath("known_hosts"),
                                    QString::fromLocal8Bit(argv[1]),
                                    0,
                                    {}};
        const auto args = RemoteSession::arguments(connection);
        require(args.contains("StrictHostKeyChecking=yes") && args.contains("BatchMode=yes") &&
                    args.contains("-a"),
                "Strict host verification and non-forwarded agent credentials");
        require(!RemoteSession::validPath("../escape") && !RemoteSession::validPath("a/../escape") &&
                    !RemoteSession::validPath("/escape") && !RemoteSession::validPath("a\nfile"),
                "Reject traversal and control-character paths");
        RemoteSession session;
        session.configure(connection);
        int events = 0, failures = 0;
        QString root, error, fingerprint;
        QJsonArray entries, tools;
        QByteArray contents, output;
        int exit = -1;
        QObject::connect(&session, &RemoteSession::failed, &app, [&](const QString& message) {
            error = message;
            ++failures;
        });
        QObject::connect(&session, &RemoteSession::connected, &app, [&](const QString& value) {
            root = value;
            ++events;
        });
        QObject::connect(&session, &RemoteSession::directoryReady, &app,
                         [&](const QString&, const QJsonArray& value) {
                             entries = value;
                             ++events;
                         });
        QObject::connect(&session, &RemoteSession::fileReady, &app,
                         [&](const QString&, const QByteArray& value, const QString& digest) {
                             contents = value;
                             fingerprint = digest;
                             ++events;
                         });
        QObject::connect(&session, &RemoteSession::saved, &app, [&](const QString&, const QString& digest) {
            fingerprint = digest;
            ++events;
        });
        QObject::connect(&session, &RemoteSession::toolsReady, &app, [&](const QJsonArray& value) {
            tools = value;
            ++events;
        });
        QObject::connect(&session, &RemoteSession::commandOutput, &app,
                         [&](const QByteArray& value) { output += value; });
        QObject::connect(&session, &RemoteSession::commandFinished, &app, [&](int value) {
            exit = value;
            ++events;
        });
        auto completed = [&](int previous) {
            return waitFor([&] { return events > previous || !error.isEmpty(); }) && error.isEmpty();
        };
        session.connectTo();
        require(completed(0) && root == QDir(project.path()).canonicalPath(), "Resolve remote project root");
        int previous = events;
        session.list();
        require(completed(previous), "Enumerate remote directory");
        bool found = false, link = false;
        for (const auto& raw : entries) {
            const auto entry = raw.toObject();
            found |= entry.value("name") == filename;
            link |= entry.value("name") == "outside-dir" && entry.value("kind") == "l";
        }
        require(found && link,
                "NUL-framed filenames preserve spaces, quotes and Unicode, while identifying links");
        previous = events;
        session.read(filename);
        require(completed(previous) && contents == "int value = 1;\n" && !fingerprint.isEmpty(),
                "Read exact remote contents and version token");
        const auto originalPermissions = QFileInfo(project.filePath(filename)).permissions();
        previous = events;
        session.write(filename, "int value = 2;\n", fingerprint);
        require(completed(previous) && read(project.filePath(filename)) == "int value = 2;\n" &&
                    QFileInfo(project.filePath(filename)).permissions() == originalPermissions,
                "Atomic save preserves existing permissions");
        write(project.filePath(filename), "concurrent remote edit\n");
        const int before = failures;
        session.write(filename, "must not overwrite\n", fingerprint);
        require(waitFor([&] { return failures > before; }) && error.contains("remote file changed") &&
                    read(project.filePath(filename)) == "concurrent remote edit\n",
                "Remote version conflict never overwrites the server");
        error.clear();
        previous = events;
        session.write("new.cpp", "new source\n", "missing");
        require(completed(previous) && read(project.filePath("new.cpp")) == "new source\n",
                "Create a remote file without overwriting an existing name");
        int fail = failures;
        session.read("outside-dir/outside.cpp");
        require(waitFor([&] { return failures > fail; }) && error.contains("escapes"),
                "Reject directory symlinks outside the project");
        error.clear();
        previous = events;
        session.discover({"sh", "veyra-nonexistent-fixture"});
        require(completed(previous) && tools.size() == 1 && tools[0].toObject().value("program") == "sh",
                "Discover only real server executables");
        previous = events;
        session.run("printf 'ready\\n'; IFS= read -r veyra_line; printf '%s\\n' \"$veyra_line\"");
        require(waitFor([&] { return output.contains("ready"); }), "Stream remote command output");
        session.sendInput("stdin value");
        require(completed(previous) && exit == 0 && output.contains("stdin value"),
                "Remote command stdin and exit status");
        session.run("sleep 30");
        QTest::qWait(50);
        QElapsedTimer stop;
        stop.start();
        session.cancel();
        require(!session.busy() && stop.elapsed() < 200, "Disconnect requests without blocking the editor");
        std::cout << "PASS: SSH transport arguments, POSIX protocol, Unicode paths, atomic saves, conflicts, "
                     "discovery and command IO\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
