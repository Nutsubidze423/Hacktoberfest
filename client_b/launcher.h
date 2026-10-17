// Sarke launcher (owner: Sandro): the app's face on a PC. Runs kvm_host.exe or kvm_client.exe in the
// background (no console window) and adds what the apps don't have themselves yet: the tray icon
// with the link state, Sarke's notification card on connect / disconnect, the Display arrangement
// window, and a settings menu (quality, frame rate, desk mode, notifications, role) that writes
// config.json and restarts the app. The apps stay unchanged (their main.cpp is Luka's); the launcher
// learns what happens from their log lines and stops them gracefully with Ctrl+Break, so windows
// come home and the virtual display is detached.
//
//   tools/sarke.cpp:  return kvm::launcher::run({.role = ..., .config_path = ...});
#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "display_layout.h"

namespace kvm::launcher {

// ---- what the app's log says (format "HH:MM:SS.mmm [level] message", kvm/log.h) ----------------

struct Connected {  // host: a client connected; client: the host's hello
    std::string name;
    std::string address;  // "ip:port" when the line has it
    int width = 0;
    int height = 0;
    double scale = 1.0;
};
struct LinkUp {  // client: the control connection is up ("connected to host ip:port")
    std::string address;
};
struct SessionEnded {};  // host: "client 'x' session ended"; client: "reconnecting in ..."
struct Searching {};     // client: looking for the host / none found yet
struct Rtt {
    double ms = 0;
};
struct Problem {  // an [error] line
    std::string text;
};
using Event = std::variant<std::monostate, Connected, LinkUp, SessionEnded, Searching, Rtt, Problem>;

Event parse_line(std::string_view line);

// ---- the launcher's own settings: %LOCALAPPDATA%\Sarke\settings.json ---------------------------

struct Settings {
    std::optional<layout::Role> role;  // which app this PC runs; asked once, switchable in the menu
    bool notify_connected = true;
    bool notify_lost = true;
};
Settings load_settings(const std::optional<std::filesystem::path>& file = std::nullopt);
bool save_settings(const Settings& settings, const std::optional<std::filesystem::path>& file = std::nullopt);

// The app for `role`: sarke_host.exe / kvm_host.exe (client: sarke_client / kvm_client) next to the
// launcher, else in the build tree (..\host_a, ..\client_b). nullopt if none is found.
std::optional<std::filesystem::path> find_app(layout::Role role, const std::filesystem::path& launcher_dir);

// ---- the launcher ------------------------------------------------------------------------------

enum class Command { Run, Quit, Restart };  // Quit / Restart: tell the launcher that is running

struct Options {
    Command command = Command::Run;
    std::optional<layout::Role> role;                  // else settings.json, else found on the network
    std::optional<std::filesystem::path> config_path;  // else the one the apps would find
    std::vector<std::wstring> app_args;                // passed to the app, e.g. --no-clipboard
};

// Runs until Quit (tray menu) or Windows ends the session. Returns the process exit code.
int run(const Options& options);

}  // namespace kvm::launcher
