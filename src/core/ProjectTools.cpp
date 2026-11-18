#include "ts/ProjectTools.hpp"
#include "ts/Path.hpp"
#include <filesystem>
#include <system_error>

namespace ts {
namespace {
namespace fs = std::filesystem;

ProcessSpec command(std::string program, std::string dir, std::initializer_list<std::string> args) {
    ProcessSpec result;
    result.valid = true;
    result.program = std::move(program);
    result.workingDirectory = std::move(dir);
    for (const auto& arg : args)
        result.arguments.pushBack(arg);
    return result;
}
}

std::string findCMakeRoot(const std::string& sourceOrDirectory) {
    if (sourceOrDirectory.empty())
        return {};
    std::error_code error;
    fs::path current = fs::absolute(pathFromUtf8(sourceOrDirectory), error);
    if (error)
        return {};
    if (fs::is_regular_file(current, error))
        current = current.parent_path();
    if (!fs::is_directory(current, error))
        return {};
    for (;;) {
        if (fs::is_regular_file(current / "CMakeLists.txt", error))
            return pathToUtf8(current.lexically_normal());
        const auto parent = current.parent_path();
        if (parent == current || parent.empty())
            return {};
        current = parent;
    }
}

CMakeProjectPlan cmakeProjectPlanFor(const std::string& sourceOrDirectory, const std::string& buildDirectory,
                                     const std::string& target, const std::string& executable) {
    CMakeProjectPlan plan;
    plan.root = findCMakeRoot(sourceOrDirectory);
    if (plan.root.empty()) {
        plan.explanation = "No CMakeLists.txt found in this directory or its ancestors.";
        return plan;
    }
    const auto root = pathFromUtf8(plan.root);
    const fs::path requested =
        buildDirectory.empty() ? root / ".veyra" / "cmake-debug" : pathFromUtf8(buildDirectory);
    const auto build = fs::absolute(requested).lexically_normal();
    plan.buildDirectory = pathToUtf8(build);
    plan.configure = command("cmake", plan.root,
                             {"-S", plan.root, "-B", plan.buildDirectory, "-DCMAKE_BUILD_TYPE=Debug",
                              "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"});
    plan.build = command("cmake", plan.root, {"--build", plan.buildDirectory, "--config", "Debug"});
    if (!target.empty()) {
        plan.build.arguments.pushBack("--target");
        plan.build.arguments.pushBack(target);
    }
    if (!executable.empty()) {
        const fs::path app = pathFromUtf8(executable);
        const fs::path resolved = app.is_absolute() ? app : (build / app);
        plan.run = command(pathToUtf8(resolved.lexically_normal()), plan.root, {});
    }
    plan.supported = true;
    plan.explanation =
        "CMake configuration may execute project-defined commands; build/run only trusted projects.";
    return plan;
}
}
