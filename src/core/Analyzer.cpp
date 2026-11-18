#include "ts/Analyzer.hpp"
#include "ts/SourceMask.hpp"
#include <algorithm>
#include <cctype>
#include <regex>

namespace ts {
namespace {
Array<std::string> linesOf(const std::string& text) {
    Array<std::string> lines;
    std::size_t from = 0;
    while (from <= text.size()) {
        std::size_t end = text.find('\n', from);
        if (end == std::string::npos) {
            lines.pushBack(text.substr(from));
            break;
        }
        lines.pushBack(text.substr(from, end - from));
        from = end + 1;
    }
    return lines;
}

void report(CodeReport& result, RiskLevel severity, int line, const char* id, const char* title,
            const char* why, const char* suggestion, int) {
    result.diagnostics.pushBack(Diagnostic{severity, line, id, title, why, suggestion});
    const std::string rule = id;
    result.diagnostics.back().confidence =
        (rule == "NO_SYNC" || rule == "SHARED_WRITE" || rule == "THREAD_LIFETIME")
            ? Confidence::Low
            : (rule == "VOLATILE" || rule == "JTHREAD" || rule == "ATOMIC" ? Confidence::High
                                                                           : Confidence::Medium);
}

bool has(const std::string& line, const char* expression) {
    struct Compiled {
        std::string pattern;
        std::regex expression;
    };

    thread_local Array<Compiled> cache;
    for (const auto& entry : cache) {
        if (entry.pattern == expression)
            return std::regex_search(line, entry.expression);
    }
    cache.pushBack(
        Compiled{expression, std::regex(expression, std::regex::ECMAScript | std::regex::optimize)});
    return std::regex_search(line, cache.back().expression);
}

struct FunctionRange {
    std::string name;
    std::size_t begin = 0, end = 0;
    std::string captures;
    std::string parameters;
};

Array<FunctionRange> functionsOf(const std::string& text) {
    Array<FunctionRange> ranges;
    const std::regex definition(R"(\b([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*(?:noexcept\s*)?\{)");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), definition); it != std::sregex_iterator{};
         ++it) {
        const auto name = (*it)[1].str();
        if (name == "if" || name == "for" || name == "while" || name == "switch" || name == "catch")
            continue;
        const auto begin = std::size_t(it->position() + it->length() - 1);
        std::size_t end = begin + 1;
        int depth = 1;
        for (; end < text.size() && depth; ++end) {
            if (text[end] == '{')
                ++depth;
            else if (text[end] == '}')
                --depth;
        }
        ranges.pushBack({name, begin, end, {}, (*it)[2].str()});
    }
    return ranges;
}

bool scopedGuardAt(const std::string& text, std::size_t begin, std::size_t position) {
    Array<std::string> scopes;
    scopes.pushBack({});
    for (std::size_t i = begin; i < position; ++i) {
        if (text[i] == '{')
            scopes.pushBack({});
        else if (text[i] == '}') {
            if (scopes.size() > 1)
                scopes.popBack();
        } else
            scopes.back() += text[i];
    }

    const std::regex guard(
        R"(\b(?:std\s*::\s*)?(?:lock_guard|scoped_lock|unique_lock)\s*(?:<[^;{}]+>)?\s+[A-Za-z_]\w*\s*[({])");
    for (const auto& scope : scopes)
        if (std::regex_search(scope, guard) && scope.find("defer_lock") == std::string::npos &&
            scope.find("adopt_lock") == std::string::npos && scope.find("try_to_lock") == std::string::npos &&
            !std::regex_search(scope, std::regex(R"(\.\s*release\s*\()")) &&
            !std::regex_search(scope, std::regex(R"(\.\s*unlock\s*\()")))
            return true;
    return false;
}

int findLine(const Array<std::string>& lines, const char* expression) {
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (has(lines[i], expression))
            return static_cast<int>(i) + 1;
    return 1;
}
}

const char* confidenceName(Confidence value) {
    return value == Confidence::High ? "high" : value == Confidence::Medium ? "medium" : "low";
}

CodeReport Analyzer::analyze(const std::string& source) const {
    CodeReport result;
    if (source.empty() || source.size() > 150000) {
        result.evaluated = false;
        return result;
    }
    const auto sanitized = maskCommentsAndStrings(source);
    const auto lines = linesOf(sanitized);
    bool hasJthread = false, hasAtomic = false;
    int threads = 0, detaches = 0, locks = 0, unlocks = 0, scoped = 0;
    for (const auto& line : lines) {
        if (has(line, R"(\bstd\s*::\s*thread\b)")) {
            ++threads;
        }
        if (has(line, R"(\bstd\s*::\s*jthread\b)")) {
            hasJthread = true;
            ++threads;
        }
        if (has(line, R"(\bstd\s*::\s*atomic\s*<|\bstd\s*::\s*atomic_[a-z]+\b)"))
            hasAtomic = true;
        if (has(line, R"(\.\s*detach\s*\()"))
            ++detaches;
        if (has(line, R"(\.\s*lock\s*\()"))
            ++locks;
        if (has(line, R"(\.\s*unlock\s*\()"))
            ++unlocks;
        if (has(line, R"(\b(?:lock_guard|scoped_lock|unique_lock|shared_lock)\b)"))
            ++scoped;
    }
    const std::regex threadPattern(R"(\bstd\s*::\s*(?:j?thread)\b)");
    threads = static_cast<int>(std::distance(
        std::sregex_iterator(sanitized.begin(), sanitized.end(), threadPattern), std::sregex_iterator{}));
    result.threadMentions = threads;
    const std::regex asyncPattern(R"(\bstd\s*::\s*async\s*\()");
    const bool hasAsync = std::regex_search(sanitized, asyncPattern);
    if (hasAsync)
        report(result, RiskLevel::Info, findLine(lines, R"(\bstd\s*::\s*async\s*\()"), "ASYNC_LAUNCH",
               "Asynchronous task using std::async",
               "Launch may be deferred; a textual call does not prove creation of a separate thread.",
               "Check std::launch, the future lifetime and when get()/wait() is called.", 0);
    result.concurrencyDetected = threads > 0 || hasAsync;
    Map<std::string, int> globalVariables;
    int braceDepth = 0;
    std::regex definition(
        R"(^\s*(?:(?:static|inline|unsigned|signed|const|volatile)\s+)*(?:int|long|bool|double|float|short|size_t|std\s*::\s*size_t)\s+([A-Za-z_]\w*)\s*(?:[=;{]|$))");
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::smatch match;
        if (braceDepth == 0 && std::regex_search(lines[i], match, definition))
            globalVariables.insertOrAssign(match[1].str(), static_cast<int>(i) + 1);
        for (char c : lines[i]) {
            if (c == '{')
                ++braceDepth;
            else if (c == '}')
                --braceDepth;
            if (braceDepth < 0)
                braceDepth = 0;
        }
    }
    auto functions = functionsOf(sanitized);
    Array<std::string> workers;
    const std::regex lambdaLaunch(R"(\bstd\s*::\s*(?:j?thread|async)\b[^;{}]*\[[^\]]*\][^{;]*\{)");
    for (auto it = std::sregex_iterator(sanitized.begin(), sanitized.end(), lambdaLaunch);
         it != std::sregex_iterator{}; ++it) {
        const auto begin = std::size_t(it->position() + it->length() - 1);
        std::size_t end = begin + 1;
        int depth = 1;
        for (; end < sanitized.size() && depth; ++end) {
            if (sanitized[end] == '{')
                ++depth;
            else if (sanitized[end] == '}')
                --depth;
        }
        const auto expression = it->str();
        const auto left = expression.find('['), right = expression.find(']', left);
        functions.pushBack({"<lambda>", begin, end, expression.substr(left + 1, right - left - 1), {}});
        workers.pushBack("<lambda>");
    }

    const std::regex launch(
        R"(\bstd\s*::\s*(?:j?thread)\s+[A-Za-z_]\w*\s*[({]\s*&?\s*([A-Za-z_]\w*)\s*[,)}])");
    for (auto it = std::sregex_iterator(sanitized.begin(), sanitized.end(), launch);
         it != std::sregex_iterator{}; ++it)
        workers.pushBack((*it)[1].str());
    const std::regex asyncLaunch(
        R"(\bstd\s*::\s*async\s*\(\s*(?:std\s*::\s*launch\s*::\s*\w+\s*,\s*)?&?([A-Za-z_]\w*)\s*[,)])");
    for (auto it = std::sregex_iterator(sanitized.begin(), sanitized.end(), asyncLaunch);
         it != std::sregex_iterator{}; ++it)
        workers.pushBack((*it)[1].str());
    const std::regex localDefinition(
        R"(\b(?:int|long|bool|double|float|short|size_t)\s+([A-Za-z_]\w*)\s*(?:[=;{]))");
    for (const auto& function : functions)
        if (function.name == "<lambda>" && function.captures.find('&') != std::string::npos) {
            for (auto it = std::sregex_iterator(sanitized.begin(), sanitized.end(), localDefinition);
                 it != std::sregex_iterator{}; ++it) {
                if (std::size_t(it->position()) >= function.begin)
                    break;
                const auto name = (*it)[1].str();
                const bool capturedAll =
                    function.captures == "&" ||
                    std::regex_search(function.captures, std::regex(R"(^\s*&\s*(?:,|$))"));
                const bool capturedName =
                    std::regex_search(function.captures, std::regex("&\\s*\\b" + name + "\\b"));
                if ((capturedAll || capturedName) && !globalVariables.contains(name))
                    globalVariables.insertOrAssign(
                        name,
                        1 + int(std::count(sanitized.begin(), sanitized.begin() + it->position(), '\n')));
            }
        }
    if (result.concurrencyDetected && !globalVariables.empty()) {
        for (const auto& entry : globalVariables) {
            const auto& var = entry.key;
            bool protectedReported = false;
            const std::regex assignment("(?:\\+\\+|--)\\s*\\b" + var + "\\b|\\b" + var +
                                        "\\s*(?:\\+\\+|--|(?:[+*/%&|^-]?=(?!=)))");
            for (auto it = std::sregex_iterator(sanitized.begin(), sanitized.end(), assignment);
                 it != std::sregex_iterator{}; ++it) {
                const auto position = std::size_t(it->position());
                const int line = 1 + int(std::count(sanitized.begin(), sanitized.begin() + position, '\n'));
                const FunctionRange* function = nullptr;
                for (const auto& range : functions)
                    if (position >= range.begin && position < range.end &&
                        (!function || range.begin > function->begin))
                        function = &range;
                bool worker = false;
                if (function)
                    for (const auto& name : workers)
                        if (name == function->name)
                            worker = true;
                if (!worker)
                    continue;
                if (function->name == "<lambda>" && globalVariables.contains(var) &&
                    !function->captures.empty() && function->captures.find('&') == std::string::npos)
                    continue;
                const std::regex shadow("\\b(?:int|long|bool|double|float|short|size_t|auto)\\s*[&*]?\\s*" +
                                        var + "\\b");
                Array<std::string> scopes;
                scopes.pushBack({});
                for (std::size_t index = function->begin;
                     index < std::min(sanitized.size(), position + it->length()); ++index) {
                    if (sanitized[index] == '{')
                        scopes.pushBack({});
                    else if (sanitized[index] == '}') {
                        if (scopes.size() > 1)
                            scopes.popBack();
                    } else
                        scopes.back() += sanitized[index];
                }
                bool shadowed = std::regex_search(function->parameters, shadow);
                for (const auto& scope : scopes)
                    shadowed = shadowed || std::regex_search(scope, shadow);
                if (shadowed)
                    continue;
                const bool guard = scopedGuardAt(sanitized, function->begin, position);
                if (guard && protectedReported)
                    continue;
                report(result, guard ? RiskLevel::Info : RiskLevel::Warning, line,
                       guard ? "PROTECTED_WRITE" : "SHARED_WRITE",
                       guard ? "Write within an RAII lock scope"
                             : "Shared-state write from a thread function",
                       ("Variable '" + var + "', function '" + function->name +
                        "' was passed to a thread/async constructor. " +
                        (guard ? "The write is in an active lexical RAII guard scope; the correct mutex and "
                                 "other accesses are not proven."
                               : "No active RAII guard was found for this write. This is a potential risk, "
                                 "not a proven data race."))
                           .c_str(),
                       "Check all accesses and lifetimes; use atomic, a shared mutex or independent data.",
                       guard ? 0 : 20);
                result.diagnostics.back().confidence = Confidence::Medium;
                if (!guard)
                    break;
                protectedReported = true;
            }
        }
    }
    const std::regex namedThread(R"(\bstd\s*::\s*thread\s+([A-Za-z_]\w*)\s*[({])");
    for (auto it = std::sregex_iterator(sanitized.begin(), sanitized.end(), namedThread);
         it != std::sregex_iterator{}; ++it) {
        const auto name = (*it)[1].str();
        if (!std::regex_search(sanitized, std::regex("\\b" + name + R"(\s*\.\s*(?:join|detach)\s*\()"))) {
            const int line = 1 + int(std::count(sanitized.begin(), sanitized.begin() + it->position(), '\n'));
            report(result, RiskLevel::Warning, line, "THREAD_LIFETIME",
                   "No completion found for a named std::thread",
                   ("Object '" + name +
                    "' has no textual join()/detach(). Ownership transfer and indirect completion are not "
                    "tracked by this heuristic.")
                       .c_str(),
                   "Use std::jthread or a guaranteed join() on every control path.", 10);
        }
    }
    if (detaches > 0 && result.concurrencyDetected)
        report(result, RiskLevel::Warning, findLine(lines, R"(\.\s*detach\s*\()"), "DETACH",
               "Detached thread",
               "detach() complicates data lifetime management and orderly application shutdown.",
               "Prefer std::jthread or explicit lifetime management.", 10);
    if (locks > unlocks && scoped == 0 && result.concurrencyDetected)
        report(result, RiskLevel::Warning, findLine(lines, R"(\.\s*lock\s*\()"), "LOCK_BALANCE",
               "Check matching lock()/unlock() calls",
               "Direct lock() calls outnumber unlock() calls in the text. Branches are not analyzed.",
               "Use std::lock_guard / std::unique_lock (RAII).", 15);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        if (result.concurrencyDetected && has(line, R"(\bvolatile\b)"))
            report(result, RiskLevel::Warning, static_cast<int>(i) + 1, "VOLATILE",
                   "volatile does not provide inter-thread synchronization",
                   "volatile creates no happens-before relation and cannot replace atomic operations.",
                   "Use std::atomic or a mutex for shared state.", 10);
        if (result.concurrencyDetected && has(line, R"(\b(?:sleep_for|usleep|Sleep)\s*\()"))
            report(result, RiskLevel::Warning, static_cast<int>(i) + 1, "SLEEP_SYNC",
                   "Check delay-based synchronization",
                   "A fixed delay does not guarantee completion of another thread's work.",
                   "Use condition_variable, future or join.", 5);
        if (result.concurrencyDetected && has(line, R"(\bwhile\s*\(\s*(?:true|1)\s*\)\s*\{\s*\})"))
            report(result, RiskLevel::Warning, static_cast<int>(i) + 1, "BUSY_WAIT",
                   "Busy wait without an exit", "The loop may consume CPU continuously.",
                   "Use blocking waiting or an appropriate termination condition.", 10);
        if (result.diagnostics.size() > 40)
            break;
    }
    if (hasJthread)
        report(result, RiskLevel::Info, findLine(lines, R"(\bstd\s*::\s*jthread\b)"), "JTHREAD",
               "RAII thread lifetime management",
               "std::jthread joins during destruction, reducing the risk of forgetting join().",
               "Consider stop_token for orderly interruption of long tasks.", 0);
    if (hasAtomic)
        report(result, RiskLevel::Info, findLine(lines, R"(\bstd\s*::\s*atomic\b)"), "ATOMIC",
               "Uses std::atomic",
               "Atomic operations help prevent races on a specific variable, but do not make complex "
               "invariants atomic.",
               "Verify memory_order choices and consistency across fields.", 0);
    if (scoped)
        report(result, RiskLevel::Info, findLine(lines, R"(\b(?:lock_guard|scoped_lock|unique_lock)\b)"),
               "RAII_LOCK", "RAII lock object found",
               "RAII synchronization releases the mutex when its scope exits.",
               "Keep critical sections short; use scoped_lock for several mutexes.", 0);
    const std::regex conditionVariable(R"(\bstd\s*::\s*condition_variable(?:_any)?\s+([A-Za-z_]\w*))");
    for (auto it = std::sregex_iterator(sanitized.begin(), sanitized.end(), conditionVariable);
         it != std::sregex_iterator{}; ++it) {
        const std::string name = (*it)[1];
        const std::regex wait("\\b" + name + R"(\s*\.\s*wait\s*\(\s*[A-Za-z_]\w*\s*\))");
        std::smatch found;
        if (std::regex_search(sanitized, found, wait)) {
            const int line = 1 + static_cast<int>(std::count(sanitized.begin(),
                                                             sanitized.begin() + found.position(), '\n'));
            report(result, RiskLevel::Warning, line, "CV_WAIT_PREDICATE",
                   "condition_variable wait without a predicate",
                   "wait(lock) permits spurious wakeups. A loop checking the condition may make this code "
                   "correct.",
                   "Use wait(lock, predicate) or check the condition in a while loop under the same mutex.",
                   5);
        }
    }
    return result;
}
}
