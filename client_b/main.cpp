// kvm_client.exe — runs on B (receives input and the window stream from A).
// Usage: kvm_client.exe [--host <A's IP>] [--config <path>] [--log <file>] [--no-kvm] [--no-clipboard]
//   --no-kvm        don't install the input hooks (A can still control B)
//   --no-clipboard  don't share copied text/images with the host

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <windows.h>

#include "kvm/clipboard.h"
#include "kvm/config.h"
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

BOOL WINAPI on_console_ctrl(DWORD) {
    g_running = false;
    return TRUE;
}

std::string narrow(std::wstring_view s) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

// Screen = B's primary monitor, in physical pixels.
proto::Hello make_hello() {
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
    if (!conn.send(make_hello())) {
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
                match_vdd_to_peer(h->screen_w, h->screen_h, kvm::display::opposite(client_side));
                windows.set_peer_screen(h->screen_w, h->screen_h);  // B's window goes to A at A's size
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

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);

    std::optional<std::filesystem::path> config_path;
    std::optional<std::string> host_override;
    bool kvm_enabled = true;
    bool clipboard_enabled = true;
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
        }
    }

    try {
        kvm::Config cfg = kvm::load_config(config_path);
        if (host_override) {
            cfg.host_ip = *host_override;
        }
        kvm::log::info("kvm_client: protocol v{}, host {}:{}. Ctrl+C to quit.", proto::kProtocolVersion, cfg.host_ip,
                       cfg.control_port);

        // client_side says where B stands next to A, so A is on the opposite side of B.
        const auto side = kvm::display::parse_side(cfg.client_side);
        if (!side || (*side != kvm::display::Side::Left && *side != kvm::display::Side::Right)) {
            kvm::log::error("config: client_side must be \"left\" or \"right\", got \"{}\"", cfg.client_side);
            return 1;
        }
        const auto host_side = kvm::display::opposite(*side);

        // KVM lives for the whole process: its hooks stay installed across reconnects.
        std::optional<kvm::KvmLink> kvm_link;
        if (kvm_enabled) {
            kvm_link.emplace(kvm::KvmOptions{.peer_side = host_side, .is_host = false});
            kvm_link->start();  // false = hooks failed (logged); A can still control B
        }
        kvm::clip::ClipboardSync clip;
        const bool clip_on = clipboard_enabled && clip.start();

        // Files (port 50502): drop window + Ctrl+Alt+Shift+F, received files land in Downloads\KVM.
        kvm::FileLink files;
        if (kvm_link) {
            // Files dragged to the edge towards the other PC go across like on one desktop (Demetre's
            // D1 + Temo's T10): caught here, sent as a drag, the cursor follows them and the drag goes
            // on over there until the button goes up. Other drags (windows, text) are left alone.
            kvm_link->set_on_drag_at_edge(
                [&files](double y, kvm::display::Point at) { files.catch_drag(y, POINT{at.x, at.y}); });
            files.set_on_drag_caught([&kvm_link](double y) { kvm_link->switch_to_peer(y); });
        }
        // Unhook before `files` goes away (declared after it, so destroyed first).
        struct DragReset {
            kvm::KvmLink* link;
            ~DragReset() {
                if (link) link->set_on_drag_at_edge(nullptr);
            }
        } drag_reset{kvm_link ? &*kvm_link : nullptr};

        // Windows A <-> B (Ctrl+Alt+Shift+W): A's windows show up in the viewer here; a window of B
        // goes to A's viewer and back.
        kvm::WindowLink windows({.jpeg_quality = cfg.jpeg_quality, .max_fps = cfg.max_fps, .peer_side = host_side});

        kvm::Hotkeys hotkeys;
        hotkeys.add('F', [&] { files.show_catcher_at_cursor(); });
        hotkeys.add('W', [&] { windows.toggle(); });
        if (hotkeys.start()) {
            kvm::log::info("Ctrl+Alt+Shift+F: drop files on the box at the cursor to send them to the other PC");
            kvm::log::info("Ctrl+Alt+Shift+W: send the window in front to the other PC, press again to bring it back");
        }

        while (g_running) {
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
