// Minimal thread-safe logging to stderr and optionally a file: kvm::log::info("connected to {}", ip);
// Never call from inside a low-level hook callback.
#pragma once

#include <windows.h>

#include <share.h>

#include <cstdio>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <string_view>

namespace kvm::log {

namespace detail {

struct State {
    std::mutex mutex;
    std::FILE* file = nullptr;
};

inline State& state() {
    static State s;
    return s;
}

inline void write(std::string_view level, const std::string& text) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    const auto line =
        std::format("{:02}:{:02}:{:02}.{:03} [{}] {}\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level, text);
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::fputs(line.c_str(), stderr);
    if (s.file) {
        std::fputs(line.c_str(), s.file);
        std::fflush(s.file);
    }
}

}  // namespace detail

// Also append every log line to `path`. Returns false if the file cannot be opened.
inline bool open_file(const std::filesystem::path& path) {
    auto& s = detail::state();
    std::lock_guard lock(s.mutex);
    if (s.file) {
        std::fclose(s.file);
        s.file = nullptr;
    }
    // _SH_DENYWR (not _wfopen_s, which denies all sharing): the log stays readable while the exe runs.
    s.file = _wfsopen(path.c_str(), L"a", _SH_DENYWR);
    return s.file != nullptr;
}

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    detail::write("info", std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    detail::write("warn", std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    detail::write("error", std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace kvm::log
