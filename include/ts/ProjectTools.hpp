#pragma once
#include "LanguageTools.hpp"
#include <string>

namespace ts {

struct CMakeProjectPlan {
    bool supported = false;
    std::string root;
    std::string buildDirectory;
    ProcessSpec configure;
    ProcessSpec build;
    ProcessSpec run;
    std::string explanation;
};

std::string findCMakeRoot(const std::string& sourceOrDirectory);
CMakeProjectPlan cmakeProjectPlanFor(const std::string& sourceOrDirectory,
                                     const std::string& buildDirectory = "", const std::string& target = "",
                                     const std::string& executable = "");

}
