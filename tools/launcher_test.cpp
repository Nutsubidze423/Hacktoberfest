// Checks client_b/launcher.h without starting anything: what each kind of kvm_host / kvm_client log
// line means, settings.json round trips, config.json key writes, and finding the apps.
// Usage: launcher_test.exe   (exit code 0 = all passed)

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <variant>

#include <windows.h>

#include "display_layout.h"
#include "kvm/config.h"
#include "launcher.h"

namespace launcher = kvm::launcher;
namespace layout = kvm::layout;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) ++g_failures;
}

template <typename T>
const T* as(const launcher::Event& e) {
    return std::get_if<T>(&e);
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void log_lines() {
    // The exact formats of host_a/main.cpp and client_b/main.cpp (kvm::log: "HH:MM:SS.mmm [level] text").
    auto e = launcher::parse_line("01:02:03.456 [info] client 'tabuladze' connected from 172.20.10.2:51234: screen 1920x1200, dpi scale 1.50\n");
    const auto* c = as<launcher::Connected>(e);
    check(c && c->name == "tabuladze" && c->address == "172.20.10.2:51234" && c->width == 1920 && c->height == 1200 &&
              c->scale == 1.5,
          "host: client connected (name, address, screen, scale)");
    e = launcher::parse_line("01:02:03.456 [info] host 'DESKTOP-7Q0UR7J': screen 1920x1080, dpi scale 1.00\r\n");
    c = as<launcher::Connected>(e);
    check(c && c->name == "DESKTOP-7Q0UR7J" && c->address.empty() && c->width == 1920 && c->scale == 1.0,
          "client: host's hello (CRLF line)");
    e = launcher::parse_line("01:02:03.456 [info] connected to host 172.20.10.3:50500");
    check(as<launcher::LinkUp>(e) && as<launcher::LinkUp>(e)->address == "172.20.10.3:50500", "client: link up");
    check(as<launcher::SessionEnded>(launcher::parse_line("01:02:03.456 [info] client 'tabuladze' session ended")) != nullptr,
          "host: session ended");
    check(as<launcher::SessionEnded>(launcher::parse_line("01:02:03.456 [info] reconnecting in 1 s...")) != nullptr,
          "client: reconnecting");
    check(as<launcher::Searching>(launcher::parse_line("01:02:03.456 [info] looking for a KVM host on the local network...")) != nullptr,
          "client: looking for the host");
    check(as<launcher::Searching>(launcher::parse_line(
              "01:02:03.456 [info] no KVM host found yet - is kvm_host running on the other PC, on the same Wi-Fi?")) != nullptr,
          "client: no host yet");
    e = launcher::parse_line("01:02:03.456 [info] link: rtt 3.25 ms, clock offset -0.4 ms");
    check(as<launcher::Rtt>(e) && as<launcher::Rtt>(e)->ms == 3.25, "rtt");
    e = launcher::parse_line("01:02:03.456 [error] config: client_side must be \"left\" or \"right\", got \"top\"");
    check(as<launcher::Problem>(e) && as<launcher::Problem>(e)->text.starts_with("config: client_side"), "error line -> problem");
    e = launcher::parse_line("01:02:03.456 [info] layout: 'tabuladze' is on the right of this PC (set on the other PC)\r\n");
    check(as<launcher::PlaceSet>(e) && as<launcher::PlaceSet>(e)->name == "tabuladze" && as<launcher::PlaceSet>(e)->side == "right",
          "layout line -> where the other PC stands");
    check(as<launcher::IdentifyAsked>(launcher::parse_line(
              "01:02:03.456 [info] identify: the other PC shows its screen numbers - showing this PC's too")) != nullptr,
          "identify line -> show this PC's numbers");
    check(std::holds_alternative<std::monostate>(launcher::parse_line("01:02:03.456 [info] video channel from 1.2.3.4")),
          "other info lines mean nothing");
    check(std::holds_alternative<std::monostate>(launcher::parse_line("  1) tabuladze  (172.20.10.2)")), "non-log lines mean nothing");
    check(std::holds_alternative<std::monostate>(launcher::parse_line("")), "empty line");
    check(std::holds_alternative<std::monostate>(launcher::parse_line("01:02:03.456 [warn] client 'x' connected from y")),
          "a half line is not a connection");
}

void files() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const auto dir = std::filesystem::path(tmp) / L"sarke_launcher_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    const auto settings = dir / L"settings.json";
    auto s = launcher::load_settings(settings);
    check(!s.role && s.notify_connected && s.notify_lost && s.auto_connect.empty(), "no settings.json: defaults");
    s.role = layout::Role::Client;
    s.notify_lost = false;
    s.auto_connect = {"DESKTOP-7Q0UR7J", "laptop"};
    check(launcher::save_settings(s, settings), "save settings");
    const auto back = launcher::load_settings(settings);
    check(back.role == layout::Role::Client && back.notify_connected && !back.notify_lost &&
              back.auto_connect == std::vector<std::string>{"DESKTOP-7Q0UR7J", "laptop"},
          "settings round trip (auto-connect list too)");

    // Both PCs list each other: only one of them may connect at once (else both become clients).
    using namespace std::chrono_literals;
    check(launcher::auto_connect_now("alpha", "Beta", 0s) && !launcher::auto_connect_now("beta", "Alpha", 0s),
          "auto-connect: the name that sorts first connects at once (no case)");
    check(!launcher::auto_connect_now("beta", "alpha", launcher::kAutoConnectYield - 1s) &&
              launcher::auto_connect_now("beta", "alpha", launcher::kAutoConnectYield),
          "auto-connect: the other one connects after kAutoConnectYield");

    const auto cfg = dir / L"config.json";
    check(layout::ensure_config(cfg).empty() && std::filesystem::exists(cfg), "ensure_config creates the file");
    const kvm::Config loaded = kvm::load_config(cfg);
    const kvm::Config defaults;
    check(loaded.host_ip == defaults.host_ip && loaded.jpeg_quality == defaults.jpeg_quality && loaded.desk == defaults.desk &&
              loaded.client_side == defaults.client_side,
          "the created file holds the apps' defaults and loads");
    const auto before = read_file(cfg);
    check(layout::ensure_config(cfg).empty() && read_file(cfg) == before, "ensure_config leaves an existing file alone");
    check(layout::save_config_key(cfg, "jpeg_quality", 90).empty() && layout::save_config_key(cfg, "desk", false).empty() &&
              layout::save_config_key(cfg, "max_fps", 0).empty(),
          "save int / bool keys");
    const kvm::Config changed = kvm::load_config(cfg);
    check(changed.jpeg_quality == 90 && !changed.desk && changed.max_fps == 0 && changed.client_side == "right",
          "the apps read the changed keys; others kept");
    const auto text = read_file(cfg);
    check(text.find("host_ip") < text.find("jpeg_quality") && text.find("jpeg_quality") < text.find("desk"), "key order kept");

    std::filesystem::create_directories(dir / L"tools");
    std::filesystem::create_directories(dir / L"host_a");
    std::ofstream(dir / L"host_a" / L"kvm_host.exe") << "x";
    check(launcher::find_app(layout::Role::Host, dir / L"tools") == dir / L"host_a" / L"kvm_host.exe", "find_app: build tree");
    check(!launcher::find_app(layout::Role::Client, dir / L"tools"), "find_app: no client -> nullopt");
    std::ofstream(dir / L"tools" / L"sarke_host.exe") << "x";
    check(launcher::find_app(layout::Role::Host, dir / L"tools") == dir / L"tools" / L"sarke_host.exe",
          "find_app: next to the launcher first, new name first");

    std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
    log_lines();
    files();
    std::printf("\n%s\n", g_failures == 0 ? "all passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
