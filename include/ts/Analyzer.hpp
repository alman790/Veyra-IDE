#pragma once
#include "Model.hpp"

namespace ts {
enum class Confidence { Low, Medium, High };
const char* confidenceName(Confidence confidence);

struct Diagnostic {
    RiskLevel severity = RiskLevel::Info;
    int line = 1;
    std::string rule;
    std::string title;
    std::string explanation;
    std::string fix;
    Confidence confidence = Confidence::Medium;
    EvidenceOrigin origin = EvidenceOrigin::Heuristic;
    Diagnostic() = default;

    Diagnostic(RiskLevel level, int sourceLine, std::string id, std::string summary, std::string reason,
               std::string recommendation)
        : severity(level), line(sourceLine), rule(std::move(id)), title(std::move(summary)),
          explanation(std::move(reason)), fix(std::move(recommendation)) {
    }
};

struct CodeReport {
    int threadMentions = 0;
    bool evaluated = true;
    bool concurrencyDetected = false;
    Array<Diagnostic> diagnostics;
    std::string disclaimer = "Heuristic text analysis, not compilation or proof that data races are absent.";
};

class Analyzer {
  public:
    CodeReport analyze(const std::string& source) const;
};
}
