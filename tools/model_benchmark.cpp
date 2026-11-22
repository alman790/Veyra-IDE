#include "ts/LocalCodeModel.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    struct Case {
        const char *language, *before, *after, *expected;
    };

    const Case cases[] = {
        {"Python", "def add(a, b):\n    return a", "\n", "+b"},
        {"Python", "def subtract(left, right):\n    return left", "\n", "-right"},
        {"Python", "def multiply(x, y):\n    return x", "\n", "*y"},
        {"Python", "def is_empty(items):\n    return len(items)", "\n", "==0"},
        {"Python", "def maximum(a, b):\n    return a if a > b else ", "\n", "b"},
        {"C++", "int add(int a, int b) {\n    return a", "\n}\n", "+b;"},
        {"C++", "int square(int value) {\n    return value", "\n}\n", "*value;"},
        {"C++", "bool empty(const std::vector<int>& items) {\n    return items.", "\n}\n", "empty();"},
        {"JavaScript", "function add(a, b) {\n    return a", "\n}\n", "+b;"},
        {"JavaScript", "function square(value) {\n    return value", "\n}\n", "*value;"},
        {"TypeScript", "function subtract(a: number, b: number): number {\n    return a", "\n}\n", "-b;"},
        {"Java", "static int add(int a, int b) {\n    return a", "\n}\n", "+b;"},
        {"Go", "func add(a int, b int) int {\n    return a", "\n}\n", "+b"},
        {"Rust", "fn add(a: i32, b: i32) -> i32 {\n    a", "\n}\n", "+b"},
        {"Ruby", "def add(a, b)\n  a", "\nend\n", "+b"},
        {"C#", "static int Add(int a, int b) {\n    return a", "\n}\n", "+b;"},
    };
    QJsonArray rows;
    std::vector<double> timings;
    int exact = 0;
    for (int round = 0; round < 2; ++round) {
        for (const auto& test : cases) {
            QElapsedTimer timer;
            timer.start();
            const auto result =
                ts::LocalCodeModel::complete(test.before, test.after, test.language, [] { return false; });
            const double elapsed = timer.nsecsElapsed() / 1e6;
            const auto normalized = QString::fromStdString(result).remove(QRegularExpression("\\s+"));
            const bool match = normalized == test.expected;
            if (round == 0)
                exact += match;
            timings.push_back(elapsed);
            QJsonObject row{{"round", round},        {"language", test.language},
                            {"before", test.before}, {"output", QString::fromStdString(result)},
                            {"exact", match},        {"milliseconds", elapsed}};
#ifndef VEYRA_MODEL_BASELINE
            const auto metrics = ts::LocalCodeModel::metrics();
            row["cached_tokens"] = metrics.cachedTokens;
            row["prompt_tokens"] = metrics.promptTokens;
            row["generated_tokens"] = metrics.generatedTokens;
            row["backend"] = QString::fromStdString(metrics.backend);
#endif
            rows.append(row);
        }
    }
    std::sort(timings.begin(), timings.end());
    const QJsonObject result{{"cases", int(std::size(cases))},
                             {"exact", exact},
                             {"median_ms", timings[timings.size() / 2]},
                             {"p95_ms", timings[std::size_t(timings.size() * 0.95)]},
                             {"runs", rows}};
    std::cout << QJsonDocument(result).toJson(QJsonDocument::Indented).constData();
}
