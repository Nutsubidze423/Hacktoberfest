// Runtime settings from config.json (see repo root).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace kvm {

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

}  // namespace kvm
