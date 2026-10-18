// kvm_client.exe — runs on B (receives input and the window stream from A).
// Usage: kvm_client.exe [--host <A's IP>] [--config <path>] [--log <file>] [--no-kvm] [--no-clipboard] [--no-desk]
//   --no-kvm        don't install the input hooks (A can still control B)
//   --no-clipboard  don't share copied text/images with the host
//   --no-desk       no desk mode (the host's screen as one more monitor); Ctrl+Alt+Shift+W uses the viewer
//   --desk-no-vdd   desk mode, but only show the host's windows (one-PC tests: the host owns the VDD)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>

#include "kvm/clipboard.h"
#include "kvm/config.h"
#include "kvm/desk_region.h"
#include "kvm/discovery.h"
#include "kvm/dpi.h"
#include "kvm/file_link.h"
#include "kvm/file_transfer.h"
#include "kvm/hotkeys.h"
#include "kvm/kvm_link.h"
#include "kvm/log.h"
#include "kvm/net.h"
#include "kvm/protocol.h"
#include "kvm/video.h"
#include "virtual_display.h"
#include "window_link.h"

namespace {

using namespace std::chrono_literals;
namespace proto = kvm::proto;

// Windows may put the VDD back on at the driver's 800x600: give it the other PC's real size, on the
// other PC's side, so the cursor crosses a same-sized screen instead of catching on a tiny one.
void match_vdd_to_peer(int width, int height, kvm::display::Side side) {
    if (auto vdd = kvm::VirtualDisplay::find(); vdd && vdd->attached()) vdd->match_peer(width, height, side);
}

constexpr auto kReconnectDelay = 1s;
constexpr auto kStatsInterval = 5s;

std::atomic<bool> g_running{true};
std::atomic<bool> g_desk{false};        // desk mode may have the VDD attached
std::atomic<bool> g_cleaned_up{false};  // wmain's objects are gone (windows home, VDD detached)

BOOL WINAPI on_console_ctrl(DWORD type) {
    g_running = false;
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        // Windows ends the process as soon as this returns: give the main thread time to bring the
        // windows home from the VDD, and never leave the VDD attached (windows would sit on an invisible monitor).
        for (int i = 0; i < 90 && !g_cleaned_up; ++i) Sleep(50);
        if (!g_cleaned_up && g_desk) {
            if (auto vdd = kvm::VirtualDisplay::find(); vdd && vdd->attached()) vdd->detach();
        }
    }
    return TRUE;
}

std::string narrow(std::wstring_view s) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

// Screen = B's primary monitor, in physical pixels.
proto::Hello make_hello(bool desk) {
    wchar_t name[256];
    DWORD name_len = static_cast<DWORD>(std::size(name));
    if (!GetComputerNameExW(ComputerNameDnsHostname, name, &name_len)) {
        name_len = 0;
    }
    const auto primary = kvm::display::primary_monitor();
    return proto::Hello{
        .screen_w = primary.rect.width(),
        .screen_h = primary.rect.height(),
        .dpi_scale = primary.scale,
        .hostname = narrow(std::wstring_view(name, name_len)),
        .desk = desk,
    };
}

proto::Clipboard to_message(const kvm::clip::Chunk& c) {
    return {.seq = c.seq, .kind = c.kind, .total = c.total, .offset = c.offset, .data = c.data};
}

kvm::clip::Chunk to_chunk(const proto::Clipboard& m) {
    return {.seq = m.seq, .kind = m.kind, .total = m.total, .offset = m.offset, .data = m.data};
}

void run_session(kvm::net::Connection& conn, const kvm::Config& cfg, kvm::KvmLink* kvm,
                 kvm::clip::ClipboardSync* clip, kvm::FileLink& files, kvm::WindowLink& windows) {
    if (!conn.send(make_hello(cfg.desk))) {
        return;
    }
    kvm::log::info("connected to host {}", conn.peer());

    // KVM + clipboard use conn while the session lasts. detach() blocks until they
    // stop using it, so it must run before conn goes away (struct dtor).
    struct Detach {
        kvm::KvmLink* kvm;
        kvm::clip::ClipboardSync* clip;
        ~Detach() {
            if (clip) clip->detach();
            if (kvm) kvm->detach();
        }
    } detach{kvm, clip};
    if (kvm) kvm->attach([&conn](const proto::Message& m) { return conn.send_json(proto::encode(m)); });
    if (clip) clip->attach([&conn](const kvm::clip::Chunk& c) { return conn.send(to_message(c)); });

    // Windows A <-> B on this video connection: A's window shows in the viewer here (Sandro's
    // VideoReceiver reads it), B's window goes to A on the same connection.
    std::shared_ptr<kvm::net::FrameChannel> video = kvm::net::connect_video(cfg.host_ip, cfg.video_port);
    if (video) {
        kvm::log::info("video channel connected ({}:{})", cfg.host_ip, cfg.video_port);
        windows.set_video(video);
    } else {
        kvm::log::warn("no video channel — is port {} open on the host?", cfg.video_port);
    }
    windows.attach([&conn](const proto::Message& m) { return conn.send_json(proto::encode(m)); }, 0, 0);
    if (auto channel = kvm::files::connect_files(cfg.host_ip, cfg.file_port, files.callbacks())) {
        kvm::log::info("file channel connected ({}:{})", cfg.host_ip, cfg.file_port);
        files.set(std::move(channel));
    } else {
        kvm::log::warn("no file channel — is port {} open on the host?", cfg.file_port);
    }

    auto next_stats = std::chrono::steady_clock::now() + kStatsInterval;
    while (g_running && conn.is_open()) {
        if (const auto msg = conn.incoming().pop_for(500ms)) {
            if (kvm && kvm->handle(*msg)) {
                // KVM: A controls B, or A's cursor came back
            } else if (windows.handle(*msg)) {
                // windows: A's window streams to the viewer here, or A's viewer controls B's window
            } else if (const auto* c = std::get_if<proto::Clipboard>(&*msg); c && clip) {
                clip->receive(to_chunk(*c));
            } else if (const auto* h = std::get_if<proto::Hello>(&*msg)) {
                kvm::log::info("host '{}': screen {}x{}, dpi scale {:.2f}", h->hostname, h->screen_w, h->screen_h,
                               h->dpi_scale);
                const auto client_side = kvm::display::parse_side(cfg.client_side).value_or(kvm::display::Side::Right);
                if (!cfg.desk) match_vdd_to_peer(h->screen_w, h->screen_h, kvm::display::opposite(client_side));
                windows.set_peer(*h);  // B's windows go to A at A's size; desk mode: A's screen = one more monitor
            } else {
                kvm::log::info("from host: {}", proto::encode(*msg).dump());
            }
        }
        if (std::chrono::steady_clock::now() >= next_stats) {
            const auto stats = conn.stats();
            kvm::log::info("link: rtt {:.2f} ms, clock offset {:.1f} ms", stats.rtt_us / 1000.0,
                           stats.clock_offset_us / 1000.0);
            if (stats.rtt_us >= 0) windows.set_rtt_ms(static_cast<int>(stats.rtt_us / 1000), stats.clock_offset_us);
            next_stats += kStatsInterval;
        }
    }
    // Control connection is gone (or we are quitting). Hand KVM and clipboard back first, so they
    // never keep using a dead link while the side channels shut down; then drop those channels
    // without waiting for the peer (abort_socket wakes their blocked calls at once).
    if (clip) clip->detach();
    if (kvm) kvm->detach();
    detach = {nullptr, nullptr};
    windows.detach();  // B's window shown on A comes home; the viewer shows the link as lost
    files.set(nullptr);
    windows.set_video(nullptr);  // closes the video channel and joins the viewer's reader
}

// Finds the host on the local network. Prefers `preferred` (the host of the last session) so that a
// reconnect after its IP changed (common on phone hotspots) finds the same PC; asks in the console
// when several hosts answer. nullopt = nobody answered.
std::optional<kvm::discovery::Host> discover_host(const std::string& preferred) {
    auto hosts = kvm::discovery::find();
    std::erase_if(hosts, [](const kvm::discovery::Host& h) {
        if (h.protocol_version == proto::kProtocolVersion) return false;
        kvm::log::warn("discovery: '{}' ({}) speaks protocol v{}, this PC v{} - update it to the same commit", h.name,
                       h.ip, h.protocol_version, proto::kProtocolVersion);
        return true;
    });
    if (hosts.empty()) return std::nullopt;
    for (const auto& h : hosts) {
        if (!preferred.empty() && h.name == preferred) return h;
    }
    if (hosts.size() == 1) return hosts.front();

    std::printf("\nKVM hosts on this network:\n");
    for (std::size_t i = 0; i < hosts.size(); ++i) {
        std::printf("  %zu) %s  (%s)\n", i + 1, hosts[i].name.c_str(), hosts[i].ip.c_str());
    }
    std::printf("Connect to [1]: ");
    std::fflush(stdout);
    std::string line;
    std::getline(std::cin, line);
    const std::size_t pick = line.empty() ? 1 : static_cast<std::size_t>(std::atoi(line.c_str()));
    return (pick >= 1 && pick <= hosts.size()) ? hosts[pick - 1] : hosts.front();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);

    std::optional<std::filesystem::path> config_path;
    std::optional<std::string> host_override;
    bool kvm_enabled = true;
    bool clipboard_enabled = true;
    bool desk_enabled = true;
    bool desk_own_vdd = true;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == L"--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == L"--host" && i + 1 < argc) {
            host_override = narrow(argv[++i]);
        } else if (arg == L"--log" && i + 1 < argc) {
            kvm::log::open_file(argv[++i]);
        } else if (arg == L"--no-kvm") {
            kvm_enabled = false;
        } else if (arg == L"--no-clipboard") {
            clipboard_enabled = false;
        } else if (arg == L"--no-desk") {
            desk_enabled = false;
        } else if (arg == L"--desk-no-vdd") {
            desk_own_vdd = false;  // e.g. host and client on one PC: only the host uses the VDD
        }
    }

    try {
        struct CleanedUp {  // first in this scope, so it is destroyed last
            ~CleanedUp() { g_cleaned_up = true; }
        } cleaned_up;
        kvm::Config cfg = kvm::load_config(config_path);
        if (host_override) {
            cfg.host_ip = *host_override;
        }
        cfg.desk = cfg.desk && desk_enabled;
        g_desk = cfg.desk && desk_own_vdd;
        const bool auto_host = cfg.host_ip.empty() || cfg.host_ip == "auto";
        if (auto_host) {
            kvm::log::info("kvm_client: protocol v{}, host: found on the local network. Ctrl+C to quit.",
                           proto::kProtocolVersion);
        } else {
            kvm::log::info("kvm_client: protocol v{}, host {}:{}. Ctrl+C to quit.", proto::kProtocolVersion, cfg.host_ip,
                           cfg.control_port);
        }

        // client_side says where B stands next to A, so A is on the opposite side of B.
        const auto side = kvm::display::parse_side(cfg.client_side);
        if (!side) {
            kvm::log::error("config: client_side must be \"left\", \"right\", \"top\" or \"bottom\", got \"{}\"", cfg.client_side);
            return 1;
        }
        const auto host_side = kvm::display::opposite(*side);

        // Desk mode (Temo): B's virtual display = A's screen; the KVM hooks and WindowLink share it.
        kvm::desk::Region desk_region;
        kvm::desk::Region* desk = cfg.desk ? &desk_region : nullptr;

        // KVM lives for the whole process: its hooks stay installed across reconnects.
        std::optional<kvm::KvmLink> kvm_link;
        if (kvm_enabled) {
            kvm_link.emplace(kvm::KvmOptions{.peer_side = host_side, .is_host = false, .desk = desk});
            kvm_link->start();  // false = hooks failed (logged); A can still control B
        }
        kvm::clip::ClipboardSync clip;
        const bool clip_on = clipboard_enabled && clip.start();

        // Files (port 50502): drop window + Ctrl+Alt+Shift+F, received files land in Downloads\KVM.
        kvm::FileLink files;
        files.set_desk_region(desk);  // desk mode: drags over the other screen drop over there
        if (kvm_link) {
            // Files dragged to the edge towards the other PC go across like on one desktop (Demetre's
            // D1 + Temo's T10): caught here, sent as a drag, the cursor follows them and the drag goes
            // on over there until the button goes up. Other drags (windows, text) are left alone.
            kvm_link->set_on_drag_at_edge(
                [&files](double y, kvm::display::Point at) { files.catch_drag(y, POINT{at.x, at.y}); });
            files.set_on_drag_caught([&kvm_link](double y) { kvm_link->switch_to_peer(y); });
            // The other PC's user let go of a drag they carried here before it ran: it drops there.
            kvm_link->set_on_drag_release([&files] { files.peer_released(); });
        }
        // Unhook before `files` goes away (declared after it, so destroyed first).
        struct DragReset {
            kvm::KvmLink* link;
            ~DragReset() {
                if (link) link->set_on_drag_at_edge(nullptr);
                if (link) link->set_on_drag_release(nullptr);
            }
        } drag_reset{kvm_link ? &*kvm_link : nullptr};
        if (clip_on) {
            // Files copied with Ctrl+C travel over the file channel and land on the peer's clipboard
            // (ClipboardSync announces them first); arriving ones go onto ours instead of Explorer.
            clip.set_on_files_copied([&files](std::vector<std::filesystem::path> p) { files.send(std::move(p)); });
            files.set_on_received_claim(
                [&clip](const std::vector<std::filesystem::path>& saved) { return clip.files_received(saved); });
        }
        // Unhook before `files` goes away (declared after it, so destroyed first).
        struct ClipReset {
            kvm::clip::ClipboardSync* clip;
            ~ClipReset() {
                if (clip) clip->set_on_files_copied(nullptr);
            }
        } clip_reset{clip_on ? &clip : nullptr};

        // Windows A <-> B (Ctrl+Alt+Shift+W): A's windows show up in the viewer here; a window of B
        // goes to A's viewer and back.
        kvm::WindowLink windows({.jpeg_quality = cfg.jpeg_quality,
                                 .max_fps = cfg.max_fps,
                                 .peer_side = host_side,
                                 .desk = desk,
                                 .own_vdd = desk_own_vdd});
        // Ctrl+Alt+Shift+Q also ends desk mode (the hooks that keep the cursor off the VDD are gone).
        if (kvm_link) kvm_link->set_on_emergency([&windows] { windows.disable_desk(); });
        struct EmergencyReset {  // declared after `windows`, so it runs before `windows` goes away
            kvm::KvmLink* link;
            ~EmergencyReset() {
                if (link) link->set_on_emergency(nullptr);
            }
        } emergency_reset{kvm_link ? &*kvm_link : nullptr};

        kvm::Hotkeys hotkeys;
        hotkeys.add('F', [&] { files.show_catcher_at_cursor(); });
        hotkeys.add('W', [&] { windows.toggle(); });
        if (hotkeys.start()) {
            kvm::log::info("Ctrl+Alt+Shift+F: drop files on the box at the cursor to send them to the other PC");
            kvm::log::info("Ctrl+Alt+Shift+W: send the window in front to the other PC, press again to bring it back");
        }

        std::string host_name;  // the host of the last session, preferred when looking again
        while (g_running) {
            if (auto_host) {
                kvm::log::info("looking for a KVM host on the local network...");
                const auto found = discover_host(host_name);
                if (!found) {
                    kvm::log::info("no KVM host found yet - is kvm_host running on the other PC, on the same Wi-Fi?");
                    std::this_thread::sleep_for(kReconnectDelay);
                    continue;
                }
                kvm::log::info("found '{}' at {}", found->name, found->ip);
                host_name = found->name;
                cfg.host_ip = found->ip;
                cfg.control_port = found->control_port;
            }
            if (auto conn = kvm::net::connect(cfg.host_ip, cfg.control_port)) {
                run_session(*conn, cfg, kvm_link ? &*kvm_link : nullptr, clip_on ? &clip : nullptr, files, windows);
            }
            if (g_running) {
                kvm::log::info("reconnecting in {} s...", kReconnectDelay.count());
                std::this_thread::sleep_for(kReconnectDelay);
            }
        }
        kvm::log::info("kvm_client: bye");
        return 0;
    } catch (const std::exception& e) {
        kvm::log::error("{}", e.what());
        return 1;
    }
}
