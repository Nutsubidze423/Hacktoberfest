// SPDX-License-Identifier: Apache-2.0

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

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "display_layout.h"
#include "kvm/discovery.h"
#include "kvm/paired.h"

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
struct PlaceSet {  // where the connected PC stands was applied ("layout: 'name' is on the right of this PC")
    std::string name;
    std::string side;
};
struct IdentifyAsked {};  // the other PC pressed Identify: show this PC's screen numbers
struct PairingCode {      // "pairing: code 042 917 with 'name' (1A2B-...) - confirm on both PCs": ask the user
    std::string code;
    std::string name;
    std::string id;  // the short device ID
};
struct PairingDone {  // any other "pairing: ..." line: the exchange is over
    bool paired = false;
    std::string text;
};
struct Unpaired {  // "unpair: 'name' unpaired this PC": the other PC unpaired this one
    std::string name;
};
struct UnpairDone {  // "unpair: 1A2B-... is no longer paired with this PC": the app did "unpair <id>"
    std::string id;  // the short device ID
};
// Another PC runs a Sarke that speaks another protocol version (docs/SECURITY.md 6.7):
//   "net: 10.0.0.5:51234: an older Sarke (protocol v2) tried to connect - ..."  (theirs = 2)
//   "version: 'DESK' at 10.0.0.5:50500 speaks protocol v4, this PC v3 - ..."
struct VersionMismatch {
    std::string name;  // empty when only the address is known
    std::string ip;
    int theirs = 0;
    int ours = 0;
};
using Event = std::variant<std::monostate, Connected, LinkUp, SessionEnded, Searching, Rtt, Problem, PlaceSet,
                           IdentifyAsked, PairingCode, PairingDone, Unpaired, UnpairDone, VersionMismatch>;

Event parse_line(std::string_view line);

// ---- the launcher's own settings: %LOCALAPPDATA%\Sarke\settings.json ---------------------------

struct Settings {
    std::optional<layout::Role> role;  // the app this PC ran last (it starts as the host again)
    bool notify_connected = true;
    bool notify_lost = true;
    // Sarke 1.1's "Connect automatically" list, by computer name. Nothing connects by name any more:
    // Auto lives in paired.json, by device ID. A PC listed here gets Auto there when it is paired,
    // and leaves this list.
    std::vector<std::string> auto_connect;
};
Settings load_settings(const std::optional<std::filesystem::path>& file = std::nullopt);
bool save_settings(const Settings& settings, const std::optional<std::filesystem::path>& file = std::nullopt);

// Auto-connect when both PCs list each other: if both connected at once, both would become clients
// and wait for a host forever. The PC whose name sorts first connects as soon as it sees the other;
// the other one gives it kAutoConnectYield, then connects itself (e.g. the first doesn't list it).
inline constexpr std::chrono::seconds kAutoConnectYield{6};
bool auto_connect_now(std::string_view self, std::string_view peer, std::chrono::steady_clock::duration seen_for);

// ---- which PCs are paired: paired.json (kvm/paired.h) -------------------------------------------

// The folder the apps keep identity.bin and paired.json in: `--data-dir <dir>` among `app_args`,
// else identity::default_dir().
std::filesystem::path data_dir(const std::vector<std::wstring>& app_args);

// The paired PC that `host` claims to be, by the device ID in its discovery answer (docs/SECURITY.md
// 6.5). The name doesn't count: a PC answering with a paired PC's name but another ID is another,
// unpaired PC. Only a claim: the TLS handshake shows who it is.
std::optional<paired::Device> paired_as(const discovery::Host& host, const std::vector<paired::Device>& paired);

// When a paired PC was last seen, for the "Paired PCs" window: "Seen 5 min ago", or "Not seen yet"
// (last_seen 0: not since pairing). Times in ms since 1970.
std::wstring seen_text(std::int64_t last_seen_ms, std::int64_t now_ms);

// The app for `role`: sarke_host.exe / kvm_host.exe (client: sarke_client / kvm_client) next to the
// launcher, else in the build tree (..\host_a, ..\client_b). nullopt if none is found.
std::optional<std::filesystem::path> find_app(layout::Role role, const std::filesystem::path& launcher_dir);

// ---- the launcher ------------------------------------------------------------------------------

enum class Command { Run, Quit, Restart };  // Quit / Restart: tell the launcher that is running

struct Options {
    Command command = Command::Run;
    std::optional<layout::Role> role;                  // else the host (see App::decide_role)
    std::optional<std::filesystem::path> config_path;  // else the one the apps would find
    std::vector<std::wstring> app_args;                // passed to the app, e.g. --no-clipboard
    // Opened by the user: show the PCs on the network right away, one click connects. false =
    // started with Windows (--background): only the tray icon (PCs set to "Auto" still connect).
    bool show_devices = true;
};

// Runs until Quit (tray menu) or Windows ends the session. Returns the process exit code.
int run(const Options& options);

}  // namespace kvm::launcher
