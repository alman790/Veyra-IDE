#include "RemoteSession.hpp"
#include "ToolDiscovery.hpp"
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>

QString RemoteSession::quote(const QString& text) {
    return '\'' + QString(text).replace('\'', "'\"'\"'") + '\'';
}

bool RemoteSession::validPath(const QString& path, bool allowEmpty) {
    if (path.isEmpty())
        return allowEmpty;
    if (path.startsWith('/'))
        return false;
    for (const auto ch : path)
        if (ch.unicode() < 32 || ch.unicode() == 127)
            return false;
    for (const auto& part : path.split('/'))
        if (part.isEmpty() || part == "." || part == "..")
            return false;
    return true;
}

QString RemoteSession::knownHostsPath() {
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/remote";
    QDir().mkpath(directory);
    return directory + "/known_hosts";
}

QString RemoteSession::sshProgram(const RemoteConnection& connection) {
    return connection.executable.isEmpty() ? ToolDiscovery::find("ssh") : connection.executable;
}

QStringList RemoteSession::arguments(const RemoteConnection& connection) {
    auto knownHosts = connection.knownHosts.isEmpty() ? knownHostsPath() : connection.knownHosts;
    auto configPath = [](QString path) { return '"' + path.replace('\\', "/").replace('"', "\\\"") + '"'; };
    QStringList result{"-T",
                       "-a",
                       "-o",
                       "BatchMode=yes",
                       "-o",
                       "StrictHostKeyChecking=yes",
                       "-o",
                       "ConnectTimeout=10",
                       "-o",
                       "ServerAliveInterval=15",
                       "-o",
                       "ServerAliveCountMax=3",
                       "-o",
                       "UserKnownHostsFile=" + configPath(knownHosts) + " " +
                           configPath(QDir::homePath() + "/.ssh/known_hosts")};
    if (connection.port > 0)
        result << "-p" << QString::number(connection.port);
    if (!connection.user.isEmpty())
        result << "-l" << connection.user;
    if (!connection.identity.isEmpty())
        result << "-i" << connection.identity;
    if (!connection.configurationFile.isEmpty())
        result << "-F" << connection.configurationFile;
    result << "--" << connection.host;
    return result;
}

RemoteSession::RemoteSession(QObject* parent) : QObject(parent) {
    tree_.configure(process_);
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        cancel();
        emit failed("The SSH request timed out. The connection was closed.");
    });
    connect(&process_, &QProcess::started, this, [this] {
        QString error;
        if (!tree_.attach(process_.processId(), error)) {
            cancel();
            emit failed(error);
            return;
        }
        if (!input_.isEmpty())
            process_.write(input_);
        input_.clear();
        if (closeInput_)
            process_.closeWriteChannel();
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
        const auto bytes = process_.readAllStandardOutput();
        if (operation_ == "run")
            emit commandOutput(bytes);
        else {
            output_ += bytes;
            if (output_.size() > 33 * 1024 * 1024) {
                cancel();
                emit failed("The remote response exceeded its size limit.");
            }
        }
    });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        const auto bytes = process_.readAllStandardError();
        if (operation_ == "run")
            emit commandOutput(bytes);
        else
            errors_ = (errors_ + bytes).right(16384);
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            busy_ = false;
            deadline_.stop();
            emit busyChanged(false);
            emit failed(process_.errorString());
        }
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                deadline_.stop();
                tree_.reset();
                busy_ = false;
                emit busyChanged(false);
                if (cancelled_)
                    return;
                if (operation_ == "run") {
                    emit commandFinished(code);
                    return;
                }
                if (status != QProcess::NormalExit || code != 0) {
                    if (code == 73)
                        emit failed(
                            "The remote file changed. Reload it before saving; no changes were uploaded.");
                    else if (code == 74)
                        emit failed("This path escapes the remote project or is a symbolic link.");
                    else
                        emit failed(QString::fromUtf8(errors_).trimmed().isEmpty()
                                        ? QString("SSH operation failed (exit %1).").arg(code)
                                        : QString::fromUtf8(errors_).trimmed());
                    return;
                }
                if (operation_ == "connect") {
                    auto root = QString::fromUtf8(output_);
                    if (root.endsWith('\n'))
                        root.chop(1);
                    if (!root.startsWith('/') || root.contains('\n') || root.contains('\r')) {
                        emit failed("The remote shell returned an invalid project path.");
                        return;
                    }
                    connection_.root = root;
                    emit connected(root);
                } else if (operation_ == "list" || operation_ == "discover") {
                    const auto fields = output_.split('\0');
                    QJsonArray entries;
                    for (int i = 0; i + 1 < fields.size(); i += 2) {
                        const auto name = QString::fromUtf8(fields[i + 1]);
                        if (operation_ == "list" && (!validPath(name) || name.contains('/')))
                            continue;
                        if (operation_ == "discover" &&
                            (name.isEmpty() || name.contains(QChar(0)) || name.contains('\n')))
                            continue;
                        if (operation_ == "list")
                            entries.append(
                                QJsonObject{{"kind", QString::fromUtf8(fields[i])}, {"name", name}});
                        else
                            entries.append(
                                QJsonObject{{"program", QString::fromUtf8(fields[i])}, {"path", name}});
                    }
                    if (operation_ == "list")
                        emit directoryReady(path_, entries);
                    else
                        emit toolsReady(entries);
                } else if (operation_ == "read") {
                    const int line = output_.indexOf('\n');
                    const auto fingerprint = QString::fromLatin1(output_.left(line)).trimmed();
                    if (line < 0 || !QRegularExpression("^[0-9]+ [0-9]+$").match(fingerprint).hasMatch()) {
                        emit failed("The remote shell returned an invalid file response.");
                        return;
                    }
                    emit fileReady(path_, output_.mid(line + 1), fingerprint);
                } else if (operation_ == "write")
                    emit saved(path_, QString::fromLatin1(output_).trimmed());
            });
}

RemoteSession::~RemoteSession() {
    blockSignals(true);
    process_.disconnect(this);
    cancel();
    process_.waitForFinished(1500);
}

void RemoteSession::configure(const RemoteConnection& connection) {
    connection_ = connection;
}

QString RemoteSession::prefix(const QString& relative, bool file) const {
    QString root = quote(connection_.root);
    if (connection_.root == "~")
        root = "\"$HOME\"";
    else if (connection_.root.startsWith("~/"))
        root = "\"$HOME\"/" + quote(connection_.root.mid(2));
    const int slash = relative.lastIndexOf('/');
    const auto directory = file ? (slash < 0 ? QString() : relative.left(slash)) : relative;
    return "CDPATH=''; veyra_root=$(cd -P " + root +
           " && pwd -P) || exit 71; "
           "cd -P \"$veyra_root\"/" +
           quote(directory) +
           " || exit 72; "
           "case \"$veyra_root\" in /) ;; *) case \"$PWD/\" in \"$veyra_root/\"*) ;; *) exit 74;; esac;; "
           "esac; ";
}

void RemoteSession::start(const QString& operation, const QString& path, const QString& script,
                          const QByteArray& input, bool closeInput, int timeout) {
    if (busy_ || process_.state() != QProcess::NotRunning) {
        emit failed("Wait for the current SSH request to finish.");
        return;
    }
    if (connection_.host.isEmpty() || connection_.host.startsWith('-') ||
        connection_.host.contains(QRegularExpression("[\\s\\x00-\\x1f]")) || connection_.root.isEmpty() ||
        connection_.root.contains('\n') || connection_.root.contains('\r') ||
        connection_.root.contains(QChar(0))) {
        emit failed("Enter a host alias and a valid remote project path.");
        return;
    }
    const auto program = sshProgram(connection_);
    if (program.isEmpty()) {
        emit failed("Install an OpenSSH client to use remote development.");
        return;
    }
    operation_ = operation;
    path_ = path;
    output_.clear();
    errors_.clear();
    input_ = input;
    closeInput_ = closeInput;
    cancelled_ = false;
    busy_ = true;
    emit busyChanged(true);
    process_.setProcessEnvironment(ToolDiscovery::environment());
    auto args = arguments(connection_);
    args << script;
    process_.start(program, args);
    if (timeout > 0)
        deadline_.start(timeout);
}

void RemoteSession::connectTo() {
    start("connect", {}, prefix({}, false) + "pwd -P");
}

void RemoteSession::list(const QString& path) {
    if (!validPath(path, true)) {
        emit failed("Choose a path inside the remote project.");
        return;
    }
    start("list", path,
          prefix(path, false) + "for veyra_entry in ./* ./.[!.]* ./..?*; do "
                                "[ -e \"$veyra_entry\" ] || [ -L \"$veyra_entry\" ] || continue; "
                                "if [ -L \"$veyra_entry\" ]; then veyra_kind=l; elif [ -d \"$veyra_entry\" "
                                "]; then veyra_kind=d; else veyra_kind=f; fi; "
                                "printf '%s\\0%s\\0' \"$veyra_kind\" \"${veyra_entry#./}\"; done");
}

void RemoteSession::read(const QString& path) {
    if (!validPath(path)) {
        emit failed("Choose a file inside the remote project.");
        return;
    }
    const auto name = quote(path.section('/', -1));
    start("read", path,
          prefix(path, true) + "[ ! -L " + name + " ] || exit 74; [ -f " + name +
              " ] || exit 72; "
              "veyra_before=$(cksum < " +
              name +
              ") || exit 72; printf '%s\\n' \"$veyra_before\"; "
              "cat " +
              name + " || exit 72; [ \"$(cksum < " + name + ")\" = \"$veyra_before\" ] || exit 73;");
}

void RemoteSession::write(const QString& path, const QByteArray& bytes, const QString& fingerprint) {
    if (!validPath(path) || bytes.size() > 32 * 1024 * 1024 ||
        (fingerprint != "missing" && !QRegularExpression("^[0-9]+ [0-9]+$").match(fingerprint).hasMatch())) {
        emit failed("The file or its remote version is invalid.");
        return;
    }
    const auto name = quote(path.section('/', -1));
    const auto check = fingerprint == "missing"
                           ? "[ ! -e " + name + " ] && [ ! -L " + name + " ]"
                           : "[ ! -L " + name + " ] && [ -f " + name + " ] && [ \"$(cksum < " + name +
                                 ")\" = " + quote(fingerprint) + " ]";
    start("write", path,
          prefix(path, true) + check +
              " || exit 73; "
              "umask 077; veyra_tmp=$(mktemp './.veyra-save.XXXXXX') || exit 75; "
              "trap 'rm -f \"$veyra_tmp\"' EXIT HUP INT TERM; "
              "if [ -f " +
              name + " ]; then cp -p " + name +
              " \"$veyra_tmp\" || exit 75; fi; "
              "cat > \"$veyra_tmp\" || exit 75; " +
              check +
              " || exit 73; "
              "veyra_saved=$(cksum < \"$veyra_tmp\") || exit 75; mv -f \"$veyra_tmp\" " +
              name +
              " || exit 75; "
              "printf '%s\\n' \"$veyra_saved\";",
          bytes);
}

void RemoteSession::run(const QString& command) {
    start("run", {}, prefix({}, false) + command, {}, false, 0);
}

void RemoteSession::discover(const QStringList& programs) {
    QStringList names;
    for (const auto& program : programs)
        if (!program.isEmpty())
            names.append(quote(program));
    if (names.isEmpty()) {
        emit toolsReady({});
        return;
    }
    start("discover", {},
          prefix({}, false) + "for veyra_program in " + names.join(' ') +
              "; do veyra_path=$(command -v \"$veyra_program\" 2>/dev/null) || continue; "
              "printf '%s\\0%s\\0' \"$veyra_program\" \"$veyra_path\"; done");
}

void RemoteSession::sendInput(const QString& line) {
    if (operation_ == "run")
        process_.write(line.toUtf8() + '\n');
}

void RemoteSession::finishInput() {
    process_.closeWriteChannel();
}

void RemoteSession::cancel() {
    cancelled_ = true;
    busy_ = false;
    deadline_.stop();
    tree_.terminate();
    process_.kill();
    emit busyChanged(false);
}
