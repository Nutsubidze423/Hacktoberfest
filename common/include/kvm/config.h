// SPDX-License-Identifier: Apache-2.0

// Runtime settings from config.json (see repo root).
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace kvm {

// Where one PC this one has met stands next to it, and when the user set that (unix ms; 0 = a
// default nobody chose). Kept in sync with that PC (proto::Layout, kvm/layout_sync.h).
struct PeerLayout {
    std::string side;  // "left" / "right" / "top" / "bottom": where that PC is, seen from this one
    std::int64_t set_at = 0;
};

struct Config {
    std::string host_ip = "auto";  // "auto" (or empty) = find the host on the local network
    std::uint16_t control_port = 50500;
    std::uint16_t video_port = 50501;
    std::uint16_t file_port = 50502;
    std::string client_side = "right";  // where B stands relative to A: "left", "right", "top" or "bottom"
    int jpeg_quality = 75;
    int max_fps = 30;
    // Desk mode: the other PC's screen is one more monitor, windows are dragged across (needs the
    // Virtual Display Driver on the PC whose windows go over). false = Ctrl+Alt+Shift+W viewer only.
    bool desk = true;
    // Every PC this one has been connected to, by computer name: where it stands. client_side is the
    // default for a PC that isn't listed yet.
    std::map<std::string, PeerLayout> peers;
};

// Looks for config.json in the working directory, then next to the executable and up to
// three parent directories of it (so build\x64-release\host_a\kvm_host.exe finds the repo copy),
// then user_config_file() (an installed Sarke: Program Files can't be written).
std::optional<std::filesystem::path> find_config_file();

// %LOCALAPPDATA%\Sarke\config.json: where an installed Sarke keeps this user's settings.
std::filesystem::path user_config_file();

// Loads the given file, or the one found by find_config_file(). Missing file or fields
// fall back to defaults; a malformed file throws std::runtime_error.
Config load_config(const std::optional<std::filesystem::path>& path = std::nullopt);

// The file load_config(path) reads: `path`, else find_config_file(), else user_config_file().
std::filesystem::path config_file_in_use(const std::optional<std::filesystem::path>& path = std::nullopt);

// Writes peers[name] into `file`, keeping every other key; creates the file if needed. False if it
// can't (logged). Thread-safe within the process.
bool save_peer_layout(const std::filesystem::path& file, const std::string& name, const PeerLayout& layout);

}  // namespace kvm
