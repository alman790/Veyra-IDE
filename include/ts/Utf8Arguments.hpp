#pragma once
#include "Structures.hpp"
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h>
#endif
namespace ts {
class Utf8Arguments {
    Array<std::string> values_;
    Array<char*> pointers_;

  public:
    Utf8Arguments(int& argc, char**& argv) {
#ifdef _WIN32
        int count = 0;
        auto** wide = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!wide)
            return;

        struct Release {
            wchar_t** data;

            ~Release() {
                LocalFree(data);
            }
        } release{wide};

        for (int i = 0; i < count; ++i) {
            const int length = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            if (length <= 0)
                return;
            std::string value(static_cast<std::size_t>(length), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, value.data(), length, nullptr, nullptr);
            value.pop_back();
            values_.pushBack(std::move(value));
        }
        for (auto& value : values_)
            pointers_.pushBack(value.data());
        pointers_.pushBack(nullptr);
        argc = count;
        argv = pointers_.begin();
#else
        (void)argc;
        (void)argv;
#endif
    }
};
}
