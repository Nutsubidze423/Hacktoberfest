// SPDX-License-Identifier: Apache-2.0

// Display arrangement (owner: Sandro): where the other PC stands next to this one, as a system.
//
// The KVM crosses at the outer edge of this PC's monitors on the peer's side (kvm::display::OuterEdge)
// and maps it proportionally onto the peer's edge, so the arrangement is one value: the side.
// It lives in config.json "client_side" = where B (the client) stands next to A (the host):
//   on A: client_side = the side the other PC is on;
//   on B: client_side = the opposite of the side the other PC is on.
// Both PCs read their own config.json, so both must agree (the window says what to set on the other).
//
// Pure logic, no UI, so tools/display_layout_test.cpp can check it. The window: display_settings.h.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "kvm/dpi.h"

namespace kvm::layout {

using display::Monitor;
using display::Rect;
using display::Side;

enum class Role { Host, Client };

// ---- this PC ------------------------------------------------------------------------------------

// This PC's real monitors (primary first, then left to right): kvm::display::monitors() without the
// Virtual Display Driver output, which is the other PC's screen in desk mode, not a screen of this PC.
std::vector<Monitor> local_monitors();

// True if `device` (Monitor::name, e.g. "\\.\DISPLAY5") is an output of the Virtual Display Driver.
bool is_virtual_display(const std::string& device);

// The number Windows shows for a monitor in Settings > Display ("\\.\DISPLAY2" -> 2), 0 if unknown.
int display_number(const std::string& device);

// This PC's name (the same one the hello message carries).
std::string computer_name();

// ---- the other PC -------------------------------------------------------------------------------

struct Peer {
    std::string hostname;  // "" = never connected
    int width = 1920;      // its primary monitor, physical pixels (hello.screen_w/h)
    int height = 1080;
    double scale = 1.0;    // hello.dpi_scale
    bool known() const { return !hostname.empty(); }
};

// The last peer seen in a hello, saved by remember_peer() in %LOCALAPPDATA%\Sarke\peer.json, so the
// arrangement shows its real size even while it is offline. nullopt if none was saved yet.
std::optional<Peer> load_peer(const std::optional<std::filesystem::path>& file = std::nullopt);
bool remember_peer(const Peer& peer, const std::optional<std::filesystem::path>& file = std::nullopt);

// ---- sides --------------------------------------------------------------------------------------

// Sides kvm_host / kvm_client accept: all four (the edge, injection, desk and VDD code handle top and
// bottom too, and both main.cpp accept them since Luka's display polish).
inline constexpr bool kVerticalSidesAccepted = true;
bool side_accepted(Side side);

// config.json client_side for "the other PC is on `peer_side` of this PC", and back.
Side client_side_for(Role role, Side peer_side);
Side peer_side_from(Role role, Side client_side);

// The side a dragged box at `p` is closest to, relative to the box `around` (in the same units).
// Compares the offsets from the centre scaled by the box size, so a wide desktop doesn't swallow
// Top/Bottom. With `vertical` false only Left / Right come out.
Side nearest_side(const Rect& around, display::Point p, bool vertical);

// ---- geometry -----------------------------------------------------------------------------------

// The pieces of the outer edge on `side` where the cursor leaves this PC (kvm::display::OuterEdge's
// segments, which it keeps private): a 1-pixel-thick rect per piece, in desktop pixels.
std::vector<Rect> outer_edge_pieces(const std::vector<Monitor>& mons, Side side);
long long outer_edge_length(const std::vector<Monitor>& mons, Side side);

// Where to draw the other PC: its own size, right outside this PC's desktop on `side`, centred
// along it (the KVM maps the whole edge onto the whole peer edge, so the offset carries no meaning).
Rect peer_rect(const std::vector<Monitor>& mons, Side side, int peer_w, int peer_h);

// ---- config.json --------------------------------------------------------------------------------

struct ConfigState {
    std::filesystem::path path;      // where it is, or where save_client_side() will create it
    bool exists = false;
    std::string client_side;         // as written, "" if missing (the apps default to "right")
    std::string host_ip;             // "" if missing
    std::string error;               // non-empty: the file exists but can't be read / parsed
    // "peers": where each PC this one has met stands, seen from here (kvm/layout_sync.h), by name.
    std::map<std::string, std::string> peer_sides;
};

// The config.json kvm_host / kvm_client would use (kvm::find_config_file()), or `path` if given.
// Without any, path = config.json next to this exe (load_config() looks there first after the
// working directory).
ConfigState read_config(const std::optional<std::filesystem::path>& path = std::nullopt);

// Writes client_side into the file, keeping every other key, its order and unknown keys. Creates the
// file if needed. Written to a temp file first and swapped in, so a crash never leaves half a file.
// Returns "" on success, else the reason.
std::string save_client_side(const std::filesystem::path& path, Side client_side);

// The same for any key (jpeg_quality, max_fps, desk, ...).
using ConfigValue = std::variant<int, bool, std::string>;
std::string save_config_key(const std::filesystem::path& path, std::string_view key, const ConfigValue& value);

// Creates `path` with every key at the apps' defaults (kvm::Config) if it doesn't exist, so users
// can see and edit them. "" on success or if it already exists, else the reason.
std::string ensure_config(const std::filesystem::path& path);

// From config.json's host_ip: Host if it is this PC (an address of one of its adapters, loopback or
// this PC's name), or "auto" / empty (the default since discovery: both PCs may share one config, so
// the user picks); Client if it names another PC.
Role guess_role(const std::string& host_ip);

// The role of the Sarke app running on this PC right now: Host if only kvm_host.exe runs, Client if
// only kvm_client.exe runs, nullopt if neither or both (e.g. a test on one PC).
std::optional<Role> running_role();

}  // namespace kvm::layout
