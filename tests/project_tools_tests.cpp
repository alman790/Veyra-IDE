#include "ts/ProjectTools.hpp"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

static void expect(bool ok, const char* reason) {
    if (!ok)
        throw std::runtime_error(reason);
}

int main() {
    try {
        QTemporaryDir directory;
        expect(directory.isValid(), "create temporary project");
        expect(QDir(directory.path()).mkpath("src"), "create source directory");
        QFile cmake(directory.filePath("CMakeLists.txt"));
        expect(cmake.open(QIODevice::WriteOnly), "create project definition");
        cmake.write("cmake_minimum_required(VERSION 3.20)\nproject(ProjectFixture LANGUAGES CXX)\n"
                    "add_executable(project_fixture src/main.cpp)\n");
        cmake.close();
        QFile entry(directory.filePath("src/main.cpp"));
        expect(entry.open(QIODevice::WriteOnly), "create project entry");
        entry.write("int main() { return 0; }\n");
        entry.close();
        const auto root = std::filesystem::weakly_canonical(directory.path().toStdString()).string();
        const auto source = root + "/src/main.cpp";
        expect(ts::findCMakeRoot(source) == root, "find project from C++ source");
        expect(ts::findCMakeRoot(root + "/src") == root, "find project from subdir");
        expect(ts::findCMakeRoot("").empty(), "empty path should not resolve");
        auto plan = ts::cmakeProjectPlanFor(source, "", "project_fixture", "project_fixture");
        expect(plan.supported, "project plan should be supported");
        expect(plan.configure.valid && plan.build.valid && plan.run.valid, "plan steps");
        expect(plan.configure.arguments.size() == 6, "configure arg count");
        expect(plan.configure.arguments[0] == "-S" && plan.configure.arguments[1] == root,
               "configure directory");
        expect(plan.configure.arguments[3] == plan.buildDirectory, "configure build path");
        expect(plan.build.arguments.size() == 6, "build arg count");
        expect(plan.build.arguments[4] == "--target" && plan.build.arguments[5] == "project_fixture",
               "explicit target");
        expect(std::filesystem::path(plan.run.program) ==
                   std::filesystem::path(plan.buildDirectory) / "project_fixture",
               "project output path");
        auto withoutRun = ts::cmakeProjectPlanFor(root);
        expect(withoutRun.supported && !withoutRun.run.valid, "project build without executable");
        std::cout << "CMake project discovery and plan tests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
