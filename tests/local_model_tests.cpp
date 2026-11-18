#include "ts/LocalCodeModel.hpp"
#include <chrono>
#include <iostream>
#include <regex>

int main() {
    const auto begin = std::chrono::steady_clock::now();
    auto result =
        ts::LocalCodeModel::complete("def add(a, b):\n    return a", "\n", "Python", [] { return false; });
    std::cout << "CONTINUATION: " << result << '\n';
    if (result.empty() || result.find("```") != std::string::npos ||
        result.find("def add") != std::string::npos)
        return 1;
    if (!ts::LocalCodeModel::complete("x", "", "text", [] { return true; }).empty())
        return 2;
    const auto choice = ts::LocalCodeModel::choose(
        "int counter; void work(){++counter;} int main(){std::thread t(work);t.join();}",
        {"Guard shared reads and writes with one synchronization policy.",
         "Check thread ownership before using detach."},
        [] { return false; });
    std::cout << "GROUNDED CHOICE: " << choice << '\n';
    if (choice < -1 || choice > 1)
        return 3;
    std::cout << "Elapsed: "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count()
              << " seconds\n";
}
