#include "../src/application/CMakeFileApi.hpp"
#include "../src/application/ProjectCreator.hpp"
#include "CMakeProjectDialog.hpp"
#include "MainWindow.hpp"
#include "ToolDiscovery.hpp"
#include "UiLanguage.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QMenuBar>
#include <QSettings>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("VeyraTests");
    app.setApplicationName("IdeWorkflow");
    QTemporaryDir settings, projects;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    const auto check = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    try {
        QString root, entry, error;
        check(
            ProjectCreator::create({projects.path(), "Sample", ts::LanguageId::Cpp, {}}, root, entry, error),
            "Create C++ project");
        check(QFileInfo::exists(root + "/CMakeLists.txt") && QFileInfo::exists(entry),
              "CMake project contents");
        QString duplicateRoot, duplicateEntry;
        check(!ProjectCreator::create({projects.path(), "Sample", ts::LanguageId::Cpp, {}}, duplicateRoot,
                                      duplicateEntry, error),
              "Never overwrite existing project");
        check(!ProjectCreator::create({projects.path(), "../escape", ts::LanguageId::Cpp, {}}, duplicateRoot,
                                      duplicateEntry, error),
              "Reject path traversal");
        QSettings().setValue("workspace/root", root);
        QSettings().setValue("lsp/enabled", false);
        MainWindow window;
        window.show();
        app.processEvents();
        check(window.findChild<CodeEditor*>("codeEditor"), "IDE has editor");
        check(window.findChild<QLabel*>("inlineThreadHint"), "IDE has live thread hints below editor");
        check(!window.findChild<QWidget*>("safetyScore") && !window.findChild<QWidget*>("railAnalysis"),
              "Legacy analytics removed");
        auto* editor = window.findChild<CodeEditor*>("codeEditor");
        check(UiLanguage::code() == "en", "First launch defaults to English");
        UiLanguage::setLanguage("ru");
        check(window.menuBar()->actions()[0]->text().contains(QString::fromUtf8("Файл")),
              "Menus switch to Russian");
        check(UiLanguage::text("Your next idea starts here").contains(QString::fromUtf8("Здесь")),
              "Welcome text has Russian translation");
        UiLanguage::setLanguage("en");
        check(window.menuBar()->actions()[0]->text().contains("File"), "Menus switch back to English");
        check(editor->isReadOnly() && window.findChild<QWidget*>("emptyEditor")->isVisible(),
              "No synthetic untitled document on startup");
        if (qEnvironmentVariableIsSet("TS_EMPTY_PREVIEW"))
            window.grab().save(qEnvironmentVariable("TS_EMPTY_PREVIEW"));
        editor->openFileRequested(entry);
        app.processEvents();
        check(!editor->isReadOnly(), "Opening a file activates the editor");

        editor->setPlainText("#include <thread>\n\nint counter = 0;\n\nvoid worker() {\n    "
                             "++counter;\n}\n\nint main() {\n    std::thread first(worker);\n    std::thread "
                             "second(worker);\n    first.join();\n    second.join();\n}\n");
        editor->jumpToLine(6);
        QElapsedTimer hints;
        hints.start();
        auto* inlineHint = window.findChild<QLabel*>("inlineThreadHint");
        while (inlineHint->isHidden() && hints.elapsed() < 5000)
            QTest::qWait(20);
        check(!inlineHint->isHidden() && inlineHint->text().contains("mutex"),
              "Thread hint appears live below code");
        editor->document()->setModified(false);
        if (qEnvironmentVariableIsSet("TS_IDE_PREVIEW"))
            window.grab().save(qEnvironmentVariable("TS_IDE_PREVIEW"));
        window.findChild<QTabBar*>("documentTabs")->tabCloseRequested(0);
        app.processEvents();
        check(window.findChild<QTabBar*>("documentTabs")->count() == 0 &&
                  window.findChild<QWidget*>("emptyEditor")->isVisible(),
              "Closing last tab shows welcome, without another untitled");
        window.close();
        app.processEvents();
        QString buildOutput;
        CMakeProjectDialog build(
            root, [&](const QString& text) { buildOutput += text; }, nullptr, ProjectAction::Run);
        build.show();
        QElapsedTimer buildDeadline;
        buildDeadline.start();
        while (!buildOutput.contains("Hello, world!") && buildDeadline.elapsed() < 30000)
            QTest::qWait(20);
        check(buildOutput.contains("Hello, world!"),
              "New project configures, builds and runs through IDE workflow");
        build.close();
        app.processEvents();
        for (const auto language : {ts::LanguageId::Python, ts::LanguageId::Java, ts::LanguageId::JavaScript,
                                    ts::LanguageId::TypeScript, ts::LanguageId::Rust, ts::LanguageId::Go}) {
            check(ProjectCreator::create({projects.path(), QString::number(int(language)), language, {}},
                                         root, entry, error),
                  "Create language project");
            check(QFileInfo::exists(entry) && QFileInfo::exists(root + "/.veyra/project.json"),
                  "Language project metadata and entry");
        }
        const auto sdkRoot = projects.path() + "/custom-sdk";
        QDir().mkpath(sdkRoot + "/bin");
#ifdef Q_OS_WIN
        const QString compilerName = "clang++.exe";
#else
        const QString compilerName = "clang++";
#endif
        QFile fakeCompiler(sdkRoot + "/bin/" + compilerName);
        check(fakeCompiler.open(QIODevice::WriteOnly), "Create SDK compiler fixture");
        fakeCompiler.write("fixture");
        fakeCompiler.close();
        fakeCompiler.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        check(ProjectCreator::create({projects.path(),
                                      "SdkProject",
                                      ts::LanguageId::Cpp,
                                      {{"name", "Custom"}, {"root", sdkRoot}, {"variable", "TEST_SDK_HOME"}}},
                                     root, entry, error),
              "Create SDK-bound project");
        QSettings().setValue("workspace/root", root);
        check(ToolDiscovery::compiler("cpp", "c++") == fakeCompiler.fileName(),
              "Build resolves project SDK compiler");
        check(ToolDiscovery::environment().value("TEST_SDK_HOME") == sdkRoot,
              "LSP/build environment carries SDK");
        const auto command = CMakeFileApi::configureCommand({root, root + "/build"});
        bool selectedCompiler = false;
        for (const auto& argument : command.arguments)
            selectedCompiler = selectedCompiler ||
                               argument == "-DCMAKE_CXX_COMPILER=" + fakeCompiler.fileName().toStdString();
        check(selectedCompiler, "CMake and its compile database use selected SDK compiler");
        check(fakeCompiler.remove(), "Remove SDK compiler fixture");
        check(ToolDiscovery::compiler("cpp", "c++").isEmpty(),
              "Missing SDK compiler never silently falls back");
        check(!CMakeFileApi::configureCommand({root, root + "/build"}).valid,
              "Missing SDK blocks CMake fallback too");
        std::cout << "PASS: project creation, collision protection, path validation and IDE shell\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
