#include "SdkInstaller.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc == 3 && QByteArray(argv[1]) == "--install-fixture") {
        QFile server(QString::fromLocal8Bit(argv[2]));
        if (!server.open(QIODevice::WriteOnly))
            return 1;
        server.write("fixture");
        server.close();
        server.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        return 0;
    }
    QApplication app(argc, argv);
    app.setOrganizationName("VeyraTests");
    app.setApplicationName("SdkInstaller-" + QUuid::createUuid().toString(QUuid::Id128));
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    const auto project = temporary.path() + "/project";
    QDir().mkpath(project);
    ToolDiscovery::DetectedSdk chosen;
    chosen.language.lspId = "fixture";
    chosen.language.name = "Fixture SDK";
    chosen.root = temporary.path();
    chosen.compiler = QCoreApplication::applicationFilePath();
    const QJsonObject recipe{
        {"steps",
         QJsonArray{QJsonObject{{"program", chosen.compiler},
                                {"arguments", QJsonArray{"--install-fixture", "{managed}/server"}}}}},
        {"server", "{managed}/server"}};
    chosen.language.installation = QJsonDocument(recipe).toJson().toStdString();
    try {
        {
            SdkInstaller preview(chosen, project, nullptr);
            preview.show();
            QTest::qWait(50);
            if (!ToolDiscovery::projectSdk(project).root.isEmpty() || !ToolDiscovery::sdks().empty())
                throw std::runtime_error("Opening preview must not install or connect tools");
            preview.close();
        }
        const auto customRoot = temporary.path() + "/custom-sdk";
        QDir().mkpath(customRoot + "/bin");
        const auto compilerFile = customRoot + "/bin/custom-compiler";
        if (!QFile::copy(QCoreApplication::applicationFilePath(), compilerFile))
            throw std::runtime_error("Create declared SDK compiler fixture");
        QFile manifest(customRoot + "/veyra-languages.json");
        if (!manifest.open(QIODevice::WriteOnly))
            return 1;
        const QJsonObject definition{{"key", "custom-fixture"},
                                     {"name", "Custom SDK"},
                                     {"extensions", QJsonArray{"customfixture"}},
                                     {"compilerCandidates", QJsonArray{"custom-compiler"}},
                                     {"runnable", true},
                                     {"build", QJsonObject{{"program", "custom-compiler"}}},
                                     {"run", QJsonObject{{"program", "{output}"}}}};
        manifest.write(QJsonDocument(QJsonObject{{"languages", QJsonArray{definition}}}).toJson());
        manifest.close();
        const auto found = ToolDiscovery::detect(customRoot);
        bool declared = false;
        for (const auto& item : found) {
            if (item.language.lspId != "custom-fixture")
                continue;
            declared = item.root == customRoot && item.compiler == compilerFile;
            if (ts::languageForPath("x.customfixture").id != ts::LanguageId::Unknown)
                throw std::runtime_error("SDK inspection must not import without Connect");
            QString error;
            if (!ToolDiscovery::adopt(item, project, error))
                throw std::runtime_error(error.toStdString());
            if (ts::languageForPath("x.customfixture").lspId != "custom-fixture")
                throw std::runtime_error("Connect imports SDK-defined language support");
        }
        if (!declared)
            throw std::runtime_error(
                "Detect a new SDK language without hardcoded extensions or compiler names");
        SdkInstaller installer(chosen, project, nullptr);
        installer.show();
        installer.findChild<QPushButton*>("installAndConnect")->click();
        QElapsedTimer deadline;
        deadline.start();
        while (installer.result() != QDialog::Accepted && deadline.elapsed() < 10000)
            QTest::qWait(20);
        if (installer.result() != QDialog::Accepted)
            throw std::runtime_error("Explicit approval must execute the installer recipe");
        if (ToolDiscovery::projectSdk(project).root != chosen.root)
            throw std::runtime_error("Connect SDK only after successful installation");
        const auto command = QProcess::splitCommand(QSettings().value("lsp/fixture/command").toString());
        if (command.isEmpty() || !QFileInfo::exists(command[0]))
            throw std::runtime_error("Connect the produced server executable");
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).removeRecursively();
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).removeRecursively();
        std::cout
            << "PASS: installer preview is inert; explicit approval installs and connects adapter tools\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
