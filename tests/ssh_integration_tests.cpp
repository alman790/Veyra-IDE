#include "RemoteSession.hpp"
#include "ToolDiscovery.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}

static bool waitFor(const std::function<bool()>& condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 12000)
        QTest::qWait(20);
    return condition();
}

static void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Write SSH server fixture");
}

static QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Read SSH server fixture");
    return file.readAll();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setOrganizationName("VeyraTests");
    app.setApplicationName("SshIntegration");
#ifdef Q_OS_WIN
    std::cout << "SKIP: loopback SSH server fixture requires a POSIX host\n";
    return 77;
#endif
    const auto ssh = ToolDiscovery::find("ssh"), keygen = ToolDiscovery::find("ssh-keygen");
    auto daemon = ToolDiscovery::find("sshd");
    if (daemon.isEmpty() && QFile::exists("/usr/sbin/sshd"))
        daemon = "/usr/sbin/sshd";
    if (ssh.isEmpty() || keygen.isEmpty() || daemon.isEmpty()) {
        std::cout << "SKIP: OpenSSH client, server or key generator is unavailable\n";
        return qEnvironmentVariableIsSet("VEYRA_REQUIRE_SSHD") ? 1 : 77;
    }
    QTemporaryDir directory(QDir::tempPath() + "/veyra ssh fixture-XXXXXX"), project;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QProcess server;
    try {
        auto key = [&](const QString& name) {
            QProcess process;
            process.start(keygen, {"-q", "-t", "ed25519", "-N", "", "-f", directory.filePath(name)});
            require(process.waitForFinished(10000) && process.exitCode() == 0,
                    "Generate isolated SSH fixture key");
        };
        key("host");
        key("client");
        QTcpServer reservation;
        require(reservation.listen(QHostAddress::LocalHost, 0), "Reserve loopback port");
        const auto port = reservation.serverPort();
        reservation.close();
        write(directory.filePath("authorized_keys"), read(directory.filePath("client.pub")));
        auto quoted = [](const QString& path) { return '"' + path + '"'; };
        const auto config =
            QString("Port %1\nListenAddress 127.0.0.1\nHostKey %2\nPidFile %3\nAuthorizedKeysFile "
                    "%4\nPasswordAuthentication no\nKbdInteractiveAuthentication no\nUsePAM no\nStrictModes "
                    "no\nLogLevel ERROR\n")
                .arg(port)
                .arg(quoted(directory.filePath("host")), quoted(directory.filePath("pid")),
                     quoted(directory.filePath("authorized_keys")));
        write(directory.filePath("sshd_config"), config.toUtf8());
        server.start(daemon, {"-D", "-e", "-f", directory.filePath("sshd_config")});
        require(server.waitForStarted(5000), "Start isolated SSH daemon");
        const bool listening = waitFor([&] {
            if (server.state() == QProcess::NotRunning)
                return true;
            QTcpSocket socket;
            socket.connectToHost(QHostAddress::LocalHost, port);
            return socket.waitForConnected(50);
        });
        if (!listening || server.state() == QProcess::NotRunning) {
            std::cerr << server.readAllStandardError().toStdString();
            return qEnvironmentVariableIsSet("VEYRA_REQUIRE_SSHD") ? 1 : 77;
        }
        const auto pub = read(directory.filePath("host.pub")).simplified().split(' ');
        write(directory.filePath("known_hosts"),
              "[127.0.0.1]:" + QByteArray::number(port) + ' ' + pub[0] + ' ' + pub[1] + '\n');
        write(directory.filePath("ssh_config"), "Host *\n  IdentitiesOnly yes\n  IdentityAgent none\n");
        const auto filename = QString::fromUtf8("hello 'server' привет.cpp");
        write(project.filePath(filename), "int value = 1;\n");
        RemoteConnection connection{"127.0.0.1",
                                    qEnvironmentVariable("USER"),
                                    project.path(),
                                    directory.filePath("client"),
                                    directory.filePath("known_hosts"),
                                    ssh,
                                    int(port),
                                    directory.filePath("ssh_config")};
        RemoteSession session;
        session.configure(connection);
        QString error, fingerprint;
        QByteArray contents;
        int events = 0;
        QObject::connect(&session, &RemoteSession::failed, &app,
                         [&](const QString& value) { error = value; });
        QObject::connect(&session, &RemoteSession::connected, &app, [&](const QString&) { ++events; });
        QObject::connect(&session, &RemoteSession::fileReady, &app,
                         [&](const QString&, const QByteArray& value, const QString& version) {
                             contents = value;
                             fingerprint = version;
                             ++events;
                         });
        QObject::connect(&session, &RemoteSession::saved, &app,
                         [&](const QString&, const QString&) { ++events; });
        session.connectTo();
        require(waitFor([&] { return events > 0 || !error.isEmpty(); }) && error.isEmpty(),
                "Authenticate through real OpenSSH with a verified host key");
        int previous = events;
        session.read(filename);
        require(waitFor([&] { return events > previous || !error.isEmpty(); }) && error.isEmpty() &&
                    contents == "int value = 1;\n",
                "Read a real remote file with a quoted Unicode path");
        previous = events;
        session.write(filename, "int value = 2;\n", fingerprint);
        require(waitFor([&] { return events > previous || !error.isEmpty(); }) && error.isEmpty() &&
                    read(project.filePath(filename)) == "int value = 2;\n",
                "Save a real remote file atomically");
        RemoteSession unknown;
        connection.knownHosts = directory.filePath("unknown_hosts");
        unknown.configure(connection);
        bool rejected = false, connected = false;
        QObject::connect(&unknown, &RemoteSession::failed, &app, [&](const QString&) { rejected = true; });
        QObject::connect(&unknown, &RemoteSession::connected, &app,
                         [&](const QString&) { connected = true; });
        unknown.connectTo();
        require(waitFor([&] { return rejected || connected; }) && rejected && !connected &&
                    !QFile::exists(connection.knownHosts),
                "Unknown host keys are rejected and never trusted automatically");
        server.terminate();
        server.waitForFinished(3000);
        std::cout
            << "PASS: real OpenSSH authentication, host verification, Unicode file IO and atomic save\n";
    } catch (const std::exception& error) {
        server.kill();
        server.waitForFinished(1000);
        std::cerr << error.what() << '\n' << server.readAllStandardError().toStdString();
        return 1;
    }
}
