#include "ts/Analyzer.hpp"
#include <iostream>
#include <stdexcept>

static bool has(const std::string& source, const std::string& rule) {
    for (const auto& hint : ts::Analyzer{}.analyze(source).diagnostics)
        if (hint.rule == rule)
            return true;
    return false;
}

static void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

int main() {
    try {
        require(has("int n; void worker() { ++n; } int main() { std::thread t(worker); t.join(); }",
                    "SHARED_WRITE"),
                "Find shared worker writes");
        require(
            !has("int n; void worker() { int n = 0; ++n; } int main() { std::thread t(worker); t.join(); }",
                 "SHARED_WRITE"),
            "Ignore shadowed local variables");
        require(!has("int n; void worker() { std::lock_guard guard(mutex); ++n; } int main() { std::thread "
                     "t(worker); t.join(); }",
                     "SHARED_WRITE"),
                "Track active lexical RAII lock");
        require(has("int n; void worker() { { std::lock_guard guard(mutex); } ++n; } int main() { "
                    "std::thread t(worker); t.join(); }",
                    "SHARED_WRITE"),
                "Expired lock does not protect write");
        require(has("int n; void worker() { std::unique_lock guard(mutex, std::try_to_lock); ++n; } int "
                    "main() { std::thread t(worker); t.join(); }",
                    "SHARED_WRITE"),
                "Try-lock does not prove lock ownership");
        require(!has("// std::thread t(worker);\nconst char* s = R\"(std::thread t(worker); t.detach();)\";",
                     "THREAD_LIFETIME"),
                "Ignore comments and raw strings");
        require(has("void f() { std::thread t([] {});", "THREAD_LIFETIME"),
                "Live hints work with unfinished outer block");
        require(!has("volatile int port; void pause() { std::this_thread::sleep_for(delay); }", "VOLATILE"),
                "No unrelated volatility warning without worker threads");
        require(has("void f() { int n = 0; std::thread t([&] { ++n; }); t.join(); }", "SHARED_WRITE"),
                "Reference capture tracks shared local state");
        require(
            !has("void f() { int n = 0; std::thread t([n]() mutable { ++n; }); t.join(); }", "SHARED_WRITE"),
            "Value capture is independent storage");
        std::cout << "PASS: scoped thread hints, live partial code and false-positive regressions\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
