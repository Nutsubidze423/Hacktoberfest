// SPDX-License-Identifier: Apache-2.0

// kvm_client.exe — runs on B (receives input and the window stream from A).
// Usage: kvm_client.exe [--host <A's IP>] [--config <path>] [--log <file>] [--no-kvm] [--no-clipboard] [--no-desk]
//   --no-kvm        don't install the input hooks (A can still control B)
//   --no-clipboard  don't share copied text/images with the host
//   --no-desk       no desk mode (the host's screen as one more monitor); Ctrl+Alt+Shift+W uses the viewer
//   --desk-no-vdd   desk mode, but only show the host's windows (one-PC tests: the host owns the VDD)
//   --pair-unknown  if the host's key isn't paired, start pairing (both users compare a code) instead of
//                   refusing it: the launcher passes it when the user picked the host
//   --data-dir <dir> --print-id --pair-with <device id> --recreate-identity: see kvm/trust.h

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
#include "kvm/pairing.h"
#include "kvm/trust.h"
#include "kvm/discovery.h"
#include "kvm/dpi.h"
#include "kvm/file_link.h"
#include "kvm/file_transfer.h"
#include "kvm/hotkeys.h"
#include "kvm/kvm_link.h"
#include "kvm/launcher_commands.h"
#include "kvm/layout_sync.h"
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
constexpr auto kHostHelloTimeout = 5s;  // the host's hello carries the side channels' token

std::atomic<bool> g_running{true};
std::string g_device_id;  // this PC's device ID (hex), sent in hello
std::atomic<bool> g_desk{false};        // desk mode may have the VDD attached
std::atomic<bool> g_cleaned_up{false};  // wmain's objects are gone (windows home, VDD detached)
std::atomic<kvm::pairing::Prompt*> g_pairing_prompt{nullptr};  // a pairing question must not hold up quitting

BOOL WINAPI on_console_ctrl(DWORD type) {
    g_running = false;
    if (auto* prompt = g_pairing_prompt.load()) prompt->cancel();
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
        .device_id = g_device_id,
    };
}

proto::Clipboard to_message(const kvm::clip::Chunk& c) {
    return {.seq = c.seq, .kind = c.kind, .total = c.total, .offset = c.offset, .data = c.data};
}

kvm::clip::Chunk to_chunk(const proto::Clipboard& m) {
    return {.seq = m.seq, .kind = m.kind, .total = m.total, .offset = m.offset, .data = m.data};
}

// How a session ended, for the reconnect loop.
enum class SessionEnd {
    Normal,  // the connection dropped or the app is quitting: reconnect
    Unpaired,  // the host unpaired this PC: never offer to pair again by itself
    VersionMismatch,  // the host speaks another protocol version: retry slowly
};

SessionEnd run_session(kvm::net::Connection& conn, const kvm::Config& cfg, kvm::KvmLink* kvm,
                       kvm::clip::ClipboardSync* clip, kvm::FileLink& files, kvm::WindowLink& windows,
                       kvm::layout_sync::Session& places, kvm::CurrentPeer& current_peer,
                       const kvm::tls::Credentials& me, const kvm::tls::DeviceId& host_id, kvm::paired::Store& paired) {
    if (!conn.send(make_hello(cfg.desk))) {
        return SessionEnd::Normal;
    }
    kvm::log::info("connected to host {}", conn.peer());
    std::string host_name;

    const auto on_host_hello = [&](const proto::Hello& h) {
        kvm::log::info("host '{}': screen {}x{}, dpi scale {:.2f}", h.hostname, h.screen_w, h.screen_h, h.dpi_scale);
        host_name = h.hostname;
        // Where the host stands, kept per PC and the same on both: before KVM and desk mode use it.
        conn.send(places.begin(h.hostname));
        // "unpair" from the launcher ends this session if it is with this host.
        current_peer.set([&conn](const proto::Message& m) { return conn.send_json(proto::encode(m)); }, host_id,
                         [&conn] {
                             conn.wait_peer_close(std::chrono::seconds(2));  // it read `unpaired` and hung up
                             conn.close();
                         });
        const auto client_side = kvm::display::parse_side(cfg.client_side).value_or(kvm::display::Side::Right);
        if (!cfg.desk) match_vdd_to_peer(h.screen_w, h.screen_h, kvm::display::opposite(client_side));
        windows.set_peer(h);  // B's windows go to A at A's size; desk mode: A's screen = one more monitor
    };
    // The host answers with its hello first: it carries the token and the ports of the session's side
    // channels (video, files; docs/SECURITY.md 6.4).
    const auto first = conn.incoming().pop_for(kHostHelloTimeout);
    const auto* host_hello = first ? std::get_if<proto::Hello>(&*first) : nullptr;
    if (!host_hello) {
        kvm::log::warn("host {}: no hello within {} s, dropping", conn.peer(), kHostHelloTimeout.count());
        return SessionEnd::Normal;
    }
    if (host_hello->protocol_version != proto::kProtocolVersion) {
        kvm::log::warn(
            "version: '{}' at {} speaks protocol v{}, this PC v{} - update both PCs to the same Sarke version",
            host_hello->hostname, conn.peer(), host_hello->protocol_version, proto::kProtocolVersion);
        return SessionEnd::VersionMismatch;
    }
    on_host_hello(*host_hello);
    const std::uint16_t video_port =
        host_hello->video_port > 0 ? static_cast<std::uint16_t>(host_hello->video_port) : cfg.video_port;
    const std::uint16_t file_port =
        host_hello->file_port > 0 ? static_cast<std::uint16_t>(host_hello->file_port) : cfg.file_port;
    const std::string token = host_hello->session_token;

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
    std::shared_ptr<kvm::net::FrameChannel> video =
        kvm::net::connect_video(cfg.host_ip, video_port, me, host_id, token);
    if (video) {
        kvm::log::info("video channel connected ({}:{})", cfg.host_ip, video_port);
        windows.set_video(video);
    } else {
        kvm::log::warn("no video channel — is port {} open on the host?", video_port);
    }
    windows.attach([&conn](const proto::Message& m) { return conn.send_json(proto::encode(m)); }, 0, 0);
    if (auto channel = kvm::files::connect_files(cfg.host_ip, file_port, me, host_id, token, files.callbacks())) {
        kvm::log::info("file channel connected ({}:{})", cfg.host_ip, file_port);
        files.set(std::move(channel));
    } else {
        kvm::log::warn("no file channel — is port {} open on the host?", file_port);
    }

    bool unpaired = false;
    auto next_stats = std::chrono::steady_clock::now() + kStatsInterval;
    while (g_running && conn.is_open() && !unpaired) {
        if (const auto msg = conn.incoming().pop_for(500ms)) {
            if (kvm && kvm->handle(*msg)) {
                // KVM: A controls B, or A's cursor came back
            } else if (windows.handle(*msg)) {
                // windows: A's window streams to the viewer here, or A's viewer controls B's window
            } else if (const auto* c = std::get_if<proto::Clipboard>(&*msg); c && clip) {
                clip->receive(to_chunk(*c));
            } else if (const auto* h = std::get_if<proto::Hello>(&*msg)) {
                on_host_hello(*h);
            } else if (const auto* l = std::get_if<proto::Layout>(&*msg)) {
                places.on_message(*l);  // the host's view of where it stands, if newer
            } else if (std::holds_alternative<proto::Identify>(*msg)) {
                kvm::log::info("identify: the other PC shows its screen numbers - showing this PC's too");
            } else if (std::holds_alternative<proto::Unpaired>(*msg)) {
                kvm::forget_unpaired_peer(paired, host_id, host_name);
                unpaired = true;  // the host closes the session
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
    current_peer.set(nullptr);
    places.end();
    windows.detach();  // B's window shown on A comes home; the viewer shows the link as lost
    files.set(nullptr);
    windows.set_video(nullptr);  // closes the video channel and joins the viewer's reader
    return unpaired ? SessionEnd::Unpaired : SessionEnd::Normal;
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
    // Started by the launcher: stdin carries its commands (kvm/launcher_commands.h), not an answer;
    // it picks a PC itself (--host).
    if (GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_PIPE) return hosts.front();

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
    bool pair_unknown = false;
    kvm::trust::Options trust_options;
    for (int i = 1; i < argc; ++i) {
        if (const int used = kvm::trust::parse_arg(trust_options, argc, argv, i)) {
            i += used - 1;
            continue;
        }
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
        } else if (arg == L"--pair-unknown") {
            pair_unknown = true;
        }
    }

    try {
        struct CleanedUp {  // first in this scope, so it is destroyed last
            ~CleanedUp() { g_cleaned_up = true; }
        } cleaned_up;
        // Who this PC is and whom it trusts (docs/SECURITY.md 6.4), before connecting anywhere.
        auto trust = kvm::trust::setup(trust_options, make_hello(false).hostname);
        if (!trust) return 1;
        if (trust_options.print_id) return 0;
        g_device_id = kvm::tls::to_hex(trust->me.id());

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
            kvm::log::info("kvm_client: protocol v{}, host {}:{}. Ctrl+C to quit.", proto::kProtocolVersion,
                           cfg.host_ip, cfg.control_port);
        }

        // client_side says where B stands next to A, so A is on the opposite side of B.
        const auto side = kvm::display::parse_side(cfg.client_side);
        if (!side) {
            kvm::log::error("config: client_side must be \"left\", \"right\", \"top\" or \"bottom\", got \"{}\"",
                            cfg.client_side);
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

        // Where the host stands: kept per PC in config.json and the same on both PCs (each session
        // agrees on it), applied to the KVM edges and the VDD while running. The launcher's Displays
        // window moves it ("layout ...") and asks for Identify on both PCs ("identify").
        kvm::layout_sync::Session places(kvm::config_file_in_use(config_path), /*this_is_host=*/false,
                                         [&](kvm::display::Side s) {
                                             if (kvm_link) kvm_link->set_peer_side(s);
                                             windows.set_peer_side(s);
                                         });
        kvm::CurrentPeer current_peer;
        kvm::pairing::Prompt pairing_prompt;  // the launcher answers "pair yes" / "pair no"
        g_pairing_prompt = &pairing_prompt;
        struct PromptReset {
            ~PromptReset() { g_pairing_prompt = nullptr; }
        } prompt_reset;
        kvm::LauncherCommands commands([&](const std::string& line) {
            kvm::handle_launcher_command(line, places, current_peer, pairing_prompt, *trust->paired);
        });

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
            if (auto pending = kvm::net::connect(cfg.host_ip, cfg.control_port, trust->me)) {
                // Pinning: the host's key must be paired before a single message of it is read.
                if (kvm::trust::is_paired(*trust->paired, *pending->stream)) {
                    const auto host_id = pending->stream->peer_id();
                    kvm::net::Connection conn(std::move(*pending));
                    const auto end =
                        run_session(conn, cfg, kvm_link ? &*kvm_link : nullptr, clip_on ? &clip : nullptr, files,
                                    windows, places, current_peer, trust->me, host_id, *trust->paired);
                    if (end == SessionEnd::Unpaired) pair_unknown = false;  // never ask to pair again by itself
                    if (end == SessionEnd::VersionMismatch) {
                        // Nothing changes until one PC is updated: don't knock every second.
                        for (int i = 0; i < 30 && g_running; ++i) std::this_thread::sleep_for(1s);
                    }
                } else if (pair_unknown) {
                    const auto paired = kvm::pairing::run(std::move(*pending), /*initiator=*/true, trust->me,
                                                          make_hello(false).hostname, *trust->paired, pairing_prompt);
                    if (paired.outcome == kvm::pairing::Outcome::Paired) continue;  // connect again, now paired
                } else {
                    kvm::log::warn(
                        "trust: {} has the key {} ('{}'), which isn't paired with this PC - connection closed. "
                        "Pick that PC in Sarke's tray to pair.",
                        pending->peer, kvm::tls::short_id(pending->stream->peer_id()), pending->stream->peer_name());
                }
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
