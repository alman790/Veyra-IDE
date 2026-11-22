#pragma once
#include <functional>
#include <string>
#include <vector>

namespace ts {
struct ModelMetrics {
    std::string backend;
    int promptTokens = 0, cachedTokens = 0, generatedTokens = 0;
    double milliseconds = 0;
};

struct SelectionFeatures {
    std::string architecture;
    std::vector<float> embedding, logits;
    std::vector<int> tokenIds;
    int vocabularySize = 0;
};

class LocalCodeModel final {
  public:
    static ModelMetrics metrics();
    static SelectionFeatures selectionFeatures(const std::string& source,
                                               const std::vector<std::string>& candidates,
                                               const std::function<bool()>& cancelled);
    static std::string complete(const std::string& before, const std::string& after,
                                const std::string& language, const std::function<bool()>& cancelled,
                                const std::string& context = {});
    static int choose(const std::string& source, const std::vector<std::string>& candidates,
                      const std::function<bool()>& cancelled);
    static std::string generate(const std::string& system, const std::string& prompt, int limit,
                                const std::function<bool()>& cancelled, const std::string& grammar = {},
                                int contextSize = 2048);
};
}
