#pragma once
#include <functional>
#include <string>
#include <vector>

namespace ts {
class LocalCodeModel final {
  public:
    static std::string complete(const std::string& before, const std::string& after,
                                const std::string& language, const std::function<bool()>& cancelled);
    static int choose(const std::string& source, const std::vector<std::string>& candidates,
                      const std::function<bool()>& cancelled);
    static std::string generate(const std::string& system, const std::string& prompt, int limit,
                                const std::function<bool()>& cancelled, const std::string& grammar = {});
};
}
