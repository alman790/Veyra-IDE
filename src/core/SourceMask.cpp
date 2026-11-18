#include "ts/SourceMask.hpp"
#include <cctype>
#include <cstddef>

namespace ts {
std::string maskCommentsAndStrings(const std::string& s) {
    std::string out = s;
    enum class Mode { Code, LineComment, BlockComment, String, Character, RawString };
    Mode mode = Mode::Code;
    std::string rawDelimiter;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i], n = i + 1 < s.size() ? s[i + 1] : '\0';
        auto blank = [&]() {
            if (c != '\n' && c != '\r')
                out[i] = ' ';
        };
        if (mode == Mode::Code) {
            if (c == '/' && n == '/') {
                mode = Mode::LineComment;
                blank();
            } else if (c == '/' && n == '*') {
                mode = Mode::BlockComment;
                blank();
            } else if (c == 'R' && n == '"') {
                std::size_t start = i + 2, open = s.find('(', start);
                if (open != std::string::npos && open - start <= 16) {
                    rawDelimiter = s.substr(start, open - start);
                    mode = Mode::RawString;
                    blank();
                }
            } else if (c == '"') {
                mode = Mode::String;
                blank();
            } else if (c == '\'' && !(i > 0 && std::isxdigit(static_cast<unsigned char>(s[i - 1])) &&
                                      std::isxdigit(static_cast<unsigned char>(n)))) {
                mode = Mode::Character;
                blank();
            }
        } else if (mode == Mode::LineComment) {
            blank();
            if (c == '\n' && !(i > 0 && s[i - 1] == '\\') && !(i > 1 && s[i - 1] == '\r' && s[i - 2] == '\\'))
                mode = Mode::Code;
        } else if (mode == Mode::BlockComment) {
            blank();
            if (c == '*' && n == '/') {
                out[i + 1] = ' ';
                ++i;
                mode = Mode::Code;
            }
        } else if (mode == Mode::RawString) {
            blank();
            if (c == ')' && s.compare(i + 1, rawDelimiter.size(), rawDelimiter) == 0 &&
                i + 1 + rawDelimiter.size() < s.size() && s[i + 1 + rawDelimiter.size()] == '"') {
                for (std::size_t j = 1; j <= rawDelimiter.size() + 1; ++j)
                    out[i + j] = ' ';
                i += rawDelimiter.size() + 1;
                mode = Mode::Code;
            }
        } else {
            blank();
            if (c == '\\' && i + 1 < s.size()) {
                ++i;
                if (s[i] != '\n' && s[i] != '\r')
                    out[i] = ' ';
                continue;
            }
            if ((mode == Mode::String && c == '"') || (mode == Mode::Character && c == '\''))
                mode = Mode::Code;
        }
    }
    return out;
}
}
