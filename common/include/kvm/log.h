// Minimal thread-safe console logging: kvm::log::info("connected to {}", ip);
// Never call from inside a low-level hook callback.
#pragma once

#include <chrono>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <string_view>

namespace kvm::log {

namespace detail {

inline std::mutex& mutex() {
    static std::mutex m;
    return m;
}

inline void write(std::string_view level, const std::string& text) {
    const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    const auto line = std::format("{:%H:%M:%S} [{}] {}\n", std::chrono::current_zone()->to_local(now), level, text);
    std::lock_guard lock(mutex());
    std::fputs(line.c_str(), stderr);
}

}  // namespace detail

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
