#include <QCoreApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto shell = QStandardPaths::findExecutable("sh");
    if (shell.isEmpty() || argc < 2)
        return 77;
    QThread::msleep(qBound(0, qEnvironmentVariableIntValue("VEYRA_SSH_FIXTURE_DELAY"), 500));
    QProcess process;
    process.setProcessChannelMode(QProcess::ForwardedChannels);
    process.setInputChannelMode(QProcess::ForwardedInputChannel);
    process.start(shell, {"-c", QString::fromLocal8Bit(argv[argc - 1])});
    if (!process.waitForStarted(5000))
        return 77;
    process.waitForFinished(-1);
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : 1;
}
