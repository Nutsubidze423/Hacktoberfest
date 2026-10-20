// SPDX-License-Identifier: Apache-2.0

#include "kvm/config.h"

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <stdexcept>

#include <windows.h>

#include <nlohmann/json.hpp>

#include "kvm/log.h"

namespace kvm {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PeerLayout, side, set_at)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Config, host_ip, control_port, video_port, file_port, client_side,
                                                jpeg_quality, max_fps, desk, peers)

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

std::filesystem::path config_file_in_use(const std::optional<std::filesystem::path>& path) {
    if (path) return *path;
    if (auto found = find_config_file()) return *found;
    return user_config_file();
}

bool save_peer_layout(const std::filesystem::path& file, const std::string& name, const PeerLayout& layout) {
    static std::mutex mutex;  // the session thread and the launcher-command thread may both save
    std::lock_guard lock(mutex);
    std::error_code ec;
    nlohmann::ordered_json j = nlohmann::ordered_json::object();
    if (std::filesystem::is_regular_file(file, ec)) {
        std::ifstream in(file);
        j = nlohmann::ordered_json::parse(in, nullptr, /*allow_exceptions=*/false);
        // Never overwrite a file that can't be read: the user's other settings are in it.
        if (j.is_discarded() || !j.is_object()) {
            log::warn("config: {} is not valid JSON - the other PC's place isn't saved", file.string());
            return false;
        }
    }
    if (!j.contains("peers") || !j["peers"].is_object()) j["peers"] = nlohmann::ordered_json::object();
    j["peers"][name] = {{"side", layout.side}, {"set_at", layout.set_at}};
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << j.dump(2) << "\n";
        if (!out) {
            log::warn("config: can't write {}", tmp.string());
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        log::warn("config: can't replace {} ({})", file.string(), GetLastError());
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace kvm
