#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QTextStream>
#include <QTimer>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() < 3)
        return 2;
    if (arguments[1] == "--input") {
        std::cout << "INPUT_READY\n" << std::flush;
        std::string line;
        std::getline(std::cin, line);
        std::cout << "INPUT_RECEIVED:" << line << std::endl;
        return line == "hello stdin" ? 0 : 3;
    }
    if (arguments[1] == "--tree") {
        QProcess child;
        child.setProcessChannelMode(QProcess::ForwardedChannels);
        QObject::connect(&child, &QProcess::started, &app, [] {
            std::cout << "CHILD_READY\n" << std::flush;
        });
        QObject::connect(&child, &QProcess::errorOccurred, &app, [&] {
            std::cerr << child.errorString().toStdString() << std::endl;
            app.exit(1);
        });
        child.start(app.applicationFilePath(), {"--heartbeat", arguments[2]});
        return app.exec();
    }
    if (arguments[1] == "--heartbeat") {
        int count = 0;
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, &app, [&] {
            QFile file(arguments[2]);
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                file.write(QByteArray::number(++count));
        });
        timer.start(20);
        return app.exec();
    }
    return 2;
}
