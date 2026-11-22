#include "ts/LocalCodeModel.hpp"
#include <atomic>
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
    const auto repeated =
        ts::LocalCodeModel::complete("def add(a, b):\n    return a", "\n", "Python", [] { return false; });
    if (repeated != result || ts::LocalCodeModel::metrics().cachedTokens <= 0)
        return 4;
    const auto cpp = ts::LocalCodeModel::complete("int add(int a, int b) {\n    return a", "\n}\n", "C++",
                                                  [] { return false; });
    if (cpp.empty() || cpp.find('}') != std::string::npos || cpp.find("b") == std::string::npos)
        return 5;
    if (ts::LocalCodeModel::generate(
            "Answer briefly.", "Reply with READY.", 8, [] { return false; }, {}, 8192)
            .empty())
        return 6;
    std::atomic<int> checks{0};
    ts::LocalCodeModel::generate("Continue the source.", std::string(3000, 'x'), 48,
                                 [&] { return ++checks > 8; });
    const auto afterCancel =
        ts::LocalCodeModel::complete("def add(a, b):\n    return a", "\n", "Python", [] { return false; });
    if (afterCancel != result)
        return 7;
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
