#include "ts/Utf8.hpp"

namespace ts {
bool validUtf8(const std::string& source) {
    for (std::size_t i = 0; i < source.size();) {
        const auto c = static_cast<unsigned char>(source[i++]);
        if (c < 0x80)
            continue;
        unsigned needed = 0;
        unsigned minSecond = 0x80, maxSecond = 0xBF;
        if (c >= 0xC2 && c <= 0xDF)
            needed = 1;
        else if (c == 0xE0) {
            needed = 2;
            minSecond = 0xA0;
        } else if (c == 0xED) {
            needed = 2;
            maxSecond = 0x9F;
        } else if (c >= 0xE1 && c <= 0xEF)
            needed = 2;
        else if (c == 0xF0) {
            needed = 3;
            minSecond = 0x90;
        } else if (c == 0xF4) {
            needed = 3;
            maxSecond = 0x8F;
        } else if (c >= 0xF1 && c <= 0xF3)
            needed = 3;
        else
            return false;
        if (source.size() - i < needed)
            return false;
        const auto next = static_cast<unsigned char>(source[i++]);
        if (next < minSecond || next > maxSecond)
            return false;
        for (unsigned j = 1; j < needed; ++j) {
            const auto part = static_cast<unsigned char>(source[i++]);
            if (part < 0x80 || part > 0xBF)
                return false;
        }
    }
    return true;
}

}
