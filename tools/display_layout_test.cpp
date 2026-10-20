// SPDX-License-Identifier: Apache-2.0

// Checks client_b/display_layout.h: side conversions host/client, drop-to-side, the crossing edge,
// where the other PC is drawn, config.json round trips (other keys kept) and peer.json; then the
// monitors of this machine.
// Usage: display_layout_test.exe   (exit code 0 = all passed)

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>

#include "display_layout.h"
#include "kvm/dpi.h"

namespace display = kvm::display;
namespace layout = kvm::layout;
using display::Monitor;
using display::Rect;
using display::Side;
using layout::Role;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) ++g_failures;
}

Monitor mon(int x, int y, int w, int h, bool primary = false) {
    Monitor m;
    m.rect = {x, y, x + w, y + h};
    m.work = m.rect;
    m.primary = primary;
    return m;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void write_file(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

void sides() {
    for (Side s : {Side::Left, Side::Right, Side::Top, Side::Bottom}) {
        const std::string name(display::to_string(s));
        check(layout::client_side_for(Role::Host, s) == s, "host: other PC on the " + name + " -> client_side " + name);
        check(layout::client_side_for(Role::Client, s) == display::opposite(s),
              "client: other PC on the " + name + " -> client_side " +
                  std::string(display::to_string(display::opposite(s))));
        for (Role r : {Role::Host, Role::Client}) {
            check(layout::peer_side_from(r, layout::client_side_for(r, s)) == s, "round trip " + name);
        }
    }
    check(layout::side_accepted(Side::Left) && layout::side_accepted(Side::Right), "left / right accepted");
    check(layout::side_accepted(Side::Top) == layout::kVerticalSidesAccepted, "top follows kVerticalSidesAccepted");

    const Rect wide{0, 0, 3840, 1080};
    check(layout::nearest_side(wide, {4200, 500}, true) == Side::Right, "drop right of a wide desktop -> right");
    check(layout::nearest_side(wide, {-300, 900}, true) == Side::Left, "drop left -> left");
    check(layout::nearest_side(wide, {1900, -400}, true) == Side::Top,
          "drop above the middle of a wide desktop -> top");
    check(layout::nearest_side(wide, {1900, 1500}, true) == Side::Bottom, "drop below -> bottom");
    check(layout::nearest_side(wide, {2000, -400}, false) == Side::Right, "no vertical: above the right half -> right");
    check(layout::nearest_side(wide, {1000, -400}, false) == Side::Left, "no vertical: above the left half -> left");
    check(layout::nearest_side({0, 0, 0, 0}, {5, 0}, true) == Side::Right, "empty box doesn't divide by zero");
}

void geometry() {
    // Two 1080p monitors side by side: the right edge is the second monitor's, 1080 px.
    const std::vector<Monitor> two{mon(0, 0, 1920, 1080, true), mon(1920, 0, 1920, 1080)};
    auto pieces = layout::outer_edge_pieces(two, Side::Right);
    check(pieces.size() == 1 && pieces[0] == Rect{3839, 0, 3840, 1080}, "two monitors: right edge = x 3839, y 0..1080");
    check(layout::outer_edge_length(two, Side::Top) == 3840, "two monitors: top edge 3840 px");
    for (Side s : {Side::Left, Side::Right, Side::Top, Side::Bottom}) {
        // Same pixel count as Demetre's OuterEdge: walk it with position() over every border pixel.
        long long hits = 0;
        const display::OuterEdge edge(two, s);
        for (const Rect& r : layout::outer_edge_pieces(two, s)) {
            for (int x = r.left; x < r.right; ++x)
                for (int y = r.top; y < r.bottom; ++y) hits += edge.position({x, y}) ? 1 : 0;
        }
        check(hits == layout::outer_edge_length(two, s),
              "pieces match OuterEdge on the " + std::string(display::to_string(s)));
    }

    // L-shape: a tall monitor on the right sticks out below the primary.
    const std::vector<Monitor> ell{mon(0, 0, 1920, 1080, true), mon(1920, 0, 1080, 1920)};
    pieces = layout::outer_edge_pieces(ell, Side::Bottom);
    check(pieces.size() == 2 && pieces[0] == Rect{0, 1079, 1920, 1080} && pieces[1] == Rect{1920, 1919, 3000, 1920},
          "L-shape: bottom edge has two pieces at different heights");
    check(layout::outer_edge_length(ell, Side::Right) == 1920, "L-shape: right edge 1920 px");

    // The other PC is drawn right outside, centred on the crossing edge.
    const auto r = layout::peer_rect(two, Side::Right, 1920, 1200);
    check(r == Rect{3840, -60, 5760, 1140}, "peer right of two monitors, centred on the edge");
    const auto l = layout::peer_rect(two, Side::Left, 1280, 800);
    check(l == Rect{-1280, 140, 0, 940}, "peer left");
    const auto t = layout::peer_rect(two, Side::Top, 1920, 1080);
    check(t == Rect{960, -1080, 2880, 0}, "peer on top, centred on 3840 px");
    const auto b = layout::peer_rect(ell, Side::Bottom, 1000, 500);
    check(b.top == 1920 && b.left == 1000, "peer below an L-shape sits under the whole desktop");
}

void config_files() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const auto dir = std::filesystem::path(tmp) / L"sarke_display_layout_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto cfg = dir / L"config.json";

    write_file(cfg,
               "{\n  \"host_ip\": \"192.168.1.10\",\n  \"client_side\": \"right\",\n  \"zz_unknown\": [1, 2],\n"
               "  \"jpeg_quality\": 80\n}\n");
    auto st = layout::read_config(cfg);
    check(st.exists && st.error.empty() && st.client_side == "right" && st.host_ip == "192.168.1.10", "read_config");
    check(layout::save_client_side(cfg, Side::Left).empty(), "save left");
    const auto text = read_file(cfg);
    st = layout::read_config(cfg);
    check(st.client_side == "left", "read back left");
    check(text.find("zz_unknown") != std::string::npos && text.find("\"jpeg_quality\": 80") != std::string::npos,
          "other and unknown keys kept");
    check(text.find("host_ip") < text.find("client_side") && text.find("client_side") < text.find("zz_unknown"),
          "key order kept");
    check(!std::filesystem::exists(dir / L"config.json.tmp"), "no temp file left over");

    write_file(cfg, "{ broken");
    st = layout::read_config(cfg);
    check(!st.error.empty(), "broken JSON is reported");
    check(!layout::save_client_side(cfg, Side::Right).empty() && read_file(cfg) == "{ broken",
          "broken JSON is never overwritten");

    const auto fresh = dir / L"sub" / L"config.json";
    st = layout::read_config(fresh);
    check(!st.exists && st.error.empty() && st.client_side.empty(), "missing file: empty state");
    check(layout::save_client_side(fresh, Side::Right).empty() && layout::read_config(fresh).client_side == "right",
          "missing file is created");

    const auto peer_file = dir / L"peer.json";
    check(!layout::load_peer(peer_file), "no peer.json: nullopt");
    check(layout::remember_peer({.hostname = "tabuladze", .width = 1920, .height = 1200, .scale = 1.5}, peer_file),
          "remember_peer");
    const auto peer = layout::load_peer(peer_file);
    check(peer && peer->hostname == "tabuladze" && peer->width == 1920 && peer->height == 1200 && peer->scale == 1.5,
          "load_peer round trip");
    write_file(peer_file, "{\"hostname\": \"x\", \"screen_w\": \"wide\"}");
    check(!layout::load_peer(peer_file), "peer.json with a wrong type: nullopt, no throw");
    check(!layout::remember_peer({}, peer_file), "an unknown peer isn't saved");

    std::filesystem::remove_all(dir);
}

void roles() {
    check(layout::guess_role("127.0.0.1") == Role::Host, "127.0.0.1 -> host");
    check(layout::guess_role("") == Role::Host, "no host_ip -> host");
    check(layout::guess_role("auto") == Role::Host, "\"auto\" (discovery, the default) -> host, the user may switch");
    check(layout::guess_role("203.0.113.7") == Role::Client, "someone else's address -> client");
    check(layout::guess_role("some-other-pc") == Role::Client, "another PC's name -> client");
    check(layout::guess_role(layout::computer_name()) == Role::Host, "this PC's own name -> host");
    const auto running = layout::running_role();
    std::printf("running here: %s\n", !running                 ? "no Sarke app (or both)"
                                      : *running == Role::Host ? "kvm_host"
                                                               : "kvm_client");
}

void this_machine() {
    display::enable_dpi_awareness();
    const auto all = display::monitors();
    const auto mons = layout::local_monitors();
    std::printf("\nthis PC '%s': %zu monitor(s), %zu without the virtual display\n", layout::computer_name().c_str(),
                all.size(), mons.size());
    for (const auto& m : mons) {
        std::printf("  %d  %s  %dx%d at (%d, %d)  %d%%%s\n", layout::display_number(m.name), m.name.c_str(),
                    m.rect.width(), m.rect.height(), m.rect.left, m.rect.top, static_cast<int>(m.scale * 100 + 0.5),
                    m.primary ? "  main" : "");
    }
    check(!mons.empty(), "at least one real monitor");
    check(layout::display_number("\\\\.\\DISPLAY12") == 12 && layout::display_number("x") == 0, "display_number");
    for (Side s : {Side::Left, Side::Right}) {
        check(layout::outer_edge_length(mons, s) > 0, "live " + std::string(display::to_string(s)) + " edge exists");
    }
    const auto st = layout::read_config();
    std::printf("config: %ls (%s), client_side '%s', host_ip '%s' -> %s\n", st.path.c_str(),
                st.exists ? "found" : "not found", st.client_side.c_str(), st.host_ip.c_str(),
                layout::guess_role(st.host_ip) == Role::Host ? "host" : "client");
}

}  // namespace

int main() {
    sides();
    geometry();
    config_files();
    roles();
    this_machine();
    std::printf("\n%s\n", g_failures == 0 ? "all passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
