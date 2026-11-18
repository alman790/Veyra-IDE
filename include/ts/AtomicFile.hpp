#pragma once
#include <string>
#include <string_view>

namespace ts {
bool writeFileAtomically(const std::string& utf8Path, std::string_view contents, std::string& error);
}
