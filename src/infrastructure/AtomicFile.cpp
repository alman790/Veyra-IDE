#include "ts/AtomicFile.hpp"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <limits>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif
namespace ts {
bool writeFileAtomically(const std::string& name, std::string_view contents, std::string& error) {
    static std::atomic<unsigned long long> serial{0};
    error.clear();
    const std::filesystem::path destination(
        std::u8string(reinterpret_cast<const char8_t*>(name.data()), name.size()));
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    auto temporary = destination;
    temporary += ".veyra-tmp-" + std::to_string(pid) + "-" + std::to_string(++serial);

    struct Cleanup {
        std::filesystem::path path;
        bool owned = false;

        ~Cleanup() {
            if (owned) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        }
    } cleanup{temporary};
#ifdef _WIN32
    HANDLE file =
        CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Cannot create temporary file: " + std::to_string(GetLastError());
        return false;
    }
    cleanup.owned = true;
    bool written = true;
    std::size_t offset = 0;
    while (offset < contents.size()) {
        DWORD bytes = 0;
        const auto size = static_cast<DWORD>(std::min<std::size_t>(contents.size() - offset, 1024 * 1024));
        if (!WriteFile(file, contents.data() + offset, size, &bytes, nullptr) || bytes == 0) {
            written = false;
            break;
        }
        offset += bytes;
    }
    written = written && FlushFileBuffers(file);
    CloseHandle(file);
    if (!written) {
        error = "Cannot finish temporary file";
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Cannot replace destination: " + std::to_string(GetLastError());
        return false;
    }
#else
    const int file = open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (file < 0) {
        error = "Cannot create temporary file: " + std::error_code(errno, std::generic_category()).message();
        return false;
    }
    cleanup.owned = true;
    bool written = true;
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const auto count = write(file, contents.data() + offset, contents.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            written = false;
            break;
        }
        offset += static_cast<std::size_t>(count);
    }
    if (fsync(file) != 0)
        written = false;
    if (close(file) != 0)
        written = false;
    if (!written) {
        error = "Cannot finish temporary file";
        return false;
    }
    std::error_code code;
    std::filesystem::rename(temporary, destination, code);
    if (code) {
        error = "Cannot replace destination: " + code.message();
        return false;
    }
#endif
    return true;
}
}
