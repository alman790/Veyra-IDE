#include "../src/application/CMakeFileApi.hpp"
#include "CodeRunner.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <stdexcept>

static void check(bool ok, const QString& why) {
    if (!ok)
        throw std::runtime_error(why.toStdString());
}

static void waitFor(bool& done) {
    QElapsedTimer time;
    time.start();
    while (!done && time.elapsed() < 120000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    check(done, "CMake stage timed out");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temp;
        const auto root = temp.path() + "/CMake проект с пробелами";
        check(QDir().mkpath(root), "Create Unicode project");
        QFile cmake(root + "/CMakeLists.txt");
        check(cmake.open(QIODevice::WriteOnly), "Create CMake project");
        cmake.write("cmake_minimum_required(VERSION 3.20)\nproject(ActualTargets LANGUAGES "
                    "CXX)\nadd_executable(logical_target main.cpp)\nset_target_properties(logical_target "
                    "PROPERTIES OUTPUT_NAME actual_binary_name)\nadd_library(helper STATIC helper.cpp)\n");
        cmake.close();
        QFile main(root + "/main.cpp");
        check(main.open(QIODevice::WriteOnly), "Create source");
        main.write("#include <cstdio>\nint main(){std::puts(\"FILE_API_EXECUTED\");}\n");
        main.close();
        QFile helper(root + "/helper.cpp");
        check(helper.open(QIODevice::WriteOnly), "Create helper");
        helper.write("int helper(){return 42;}\n");
        helper.close();
        CodeRunner runner;
        QString output, error;
        bool done = false, success = false;
        auto printed = [&](const QString& text) { output += text; };
        auto finished = [&](bool ok, int) {
            success = ok;
            done = true;
        };
        for (const auto& config : {QString("Debug"), QString("Release")}) {
            CMakeSettings settings{root, root + "/build-" + config, {}, config, {}};
            if (!QStandardPaths::findExecutable("ninja").isEmpty())
                settings.generator = "Ninja Multi-Config";
            check(CMakeFileApi::prepareQuery(settings.build, error), error);
            ts::Array<ts::ProcessSpec> steps;
            steps.pushBack(CMakeFileApi::configureCommand(settings));
            done = false;
            check(runner.startCommands(steps, false, printed, finished, error), error);
            waitFor(done);
            check(success, "Native configure: " + output);
            const auto targets = CMakeFileApi::readTargets(settings.build, config, error);
            check(error.isEmpty() && targets.size() == 2, error);
            CMakeTarget selected;
            for (const auto& target : targets)
                if (target.name == "logical_target")
                    selected = target;
            check(selected.type == "EXECUTABLE" && selected.artifact.contains("actual_binary_name"),
                  "Artifact comes from File API, not guessed target name");
            if (settings.generator == "Ninja Multi-Config")
                check(selected.artifact.contains(config + '/'), "Multi-config artifact directory");
            steps.clear();
            steps.pushBack(CMakeFileApi::buildCommand(settings, selected.name));
            steps.pushBack(CMakeFileApi::runCommand(settings, selected));
            done = false;
            output.clear();
            check(runner.startCommands(steps, true, printed, finished, error), error);
            waitFor(done);
            check(success && output.contains("FILE_API_EXECUTED"),
                  "Build and execute actual artifact: " + output);
            check(QFileInfo::exists(selected.artifact), "Native artifact exists");
        }
        std::cout << "PASS: native CMake configure/File API/target "
                     "discovery/Debug+Release/multi-config/build/run with Unicode and spaces\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
