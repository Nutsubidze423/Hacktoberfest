#include "kvm/config.h"

#include <cstdlib>
#include <fstream>
#include <stdexcept>

#include <windows.h>

#include <nlohmann/json.hpp>

#include "kvm/log.h"

namespace kvm {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Config, host_ip, control_port, video_port, file_port, client_side,
                                                jpeg_quality, max_fps, desk)

namespace {

constexpr wchar_t kConfigName[] = L"config.json";

std::filesystem::path executable_dir() {
    wchar_t buf[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(std::wstring(buf, len)).parent_path();
}

}  // namespace

std::optional<std::filesystem::path> find_config_file() {
    if (auto cwd = std::filesystem::current_path() / kConfigName; std::filesystem::exists(cwd)) {
        return cwd;
    }
    auto dir = executable_dir();
    for (int depth = 0; depth <= 3 && !dir.empty(); ++depth, dir = dir.parent_path()) {
        if (auto candidate = dir / kConfigName; std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    if (auto user = user_config_file(); !user.empty() && std::filesystem::exists(user)) return user;
    return std::nullopt;
}

std::filesystem::path user_config_file() {
    wchar_t* local = nullptr;
    std::size_t length = 0;
    std::filesystem::path out;
    if (_wdupenv_s(&local, &length, L"LOCALAPPDATA") == 0 && local && *local) {
        out = std::filesystem::path(local) / L"Sarke" / kConfigName;
    }
    std::free(local);
    return out;
}

Config load_config(const std::optional<std::filesystem::path>& path) {
    const auto file = path ? path : find_config_file();
    if (!file) {
        log::warn("config.json not found, using defaults");
        return {};
    }

    std::ifstream in(*file);
    if (!in) {
        throw std::runtime_error("cannot open " + file->string());
    }
    try {
        Config cfg = nlohmann::json::parse(in).get<Config>();
        log::info("config: {}", file->string());
        return cfg;
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error("invalid " + file->string() + ": " + e.what());
    }
}

}  // namespace kvm
