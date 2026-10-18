// kvm_host.exe — runs on A (owns the physical mouse/keyboard, streams a window).
// Usage: kvm_host.exe [--config <path>] [--log <file>] [--no-kvm] [--no-clipboard] [--no-desk]
//                     [--test-video [--test-video-kb N] [--test-video-fps N]]
//   --no-kvm      don't install the input hooks (e.g. when only testing the video stream)
//   --no-clipboard  don't share copied text/images with the client
//   --no-desk     no desk mode (the client's screen as one more monitor); Ctrl+Alt+Shift+W uses the viewer
//   --test-video  send synthetic frames (codec "test", random bytes, default 250 KB at 30 fps) so
//                 network throughput and video latency can be measured before capture/viewer exist

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
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

constexpr auto kHelloTimeout = 3s;
constexpr auto kStatsInterval = 5s;

struct Options {
    std::optional<std::filesystem::path> config_path;
    bool kvm_enabled = true;
    bool clipboard = true;
    bool desk = true;  // --no-desk, config "desk"
    bool test_video = false;
    int test_video_kb = 250;
    int test_video_fps = 30;
};

std::atomic<bool> g_running{true};
std::atomic<kvm::net::Server*> g_server{nullptr};
std::atomic<kvm::net::VideoServer*> g_video_server{nullptr};
std::atomic<kvm::files::FileServer*> g_file_server{nullptr};
std::atomic<bool> g_desk{false};          // desk mode may have the VDD attached
std::atomic<bool> g_cleaned_up{false};    // wmain's objects are gone (windows home, VDD detached)

BOOL WINAPI on_console_ctrl(DWORD type) {
    g_running = false;
    if (auto* server = g_server.load()) {
        server->stop();
    }
    if (auto* video = g_video_server.load()) {
        video->stop();
    }
    if (auto* files = g_file_server.load()) {
        files->stop();
    }
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

// The client's current video connection (port 50501). Replaced whenever the client reconnects.
class VideoSlot {
public:
    void set(std::shared_ptr<kvm::net::FrameChannel> channel) {
        std::lock_guard lock(mutex_);
        if (channel_) {
            channel_->close();
        }
        channel_ = std::move(channel);
    }
    std::shared_ptr<kvm::net::FrameChannel> get() const {
        std::lock_guard lock(mutex_);
        return channel_;
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<kvm::net::FrameChannel> channel_;
};

void accept_video_loop(kvm::net::VideoServer& server, VideoSlot& slot, kvm::WindowLink& windows) {
    while (g_running) {
        auto channel = server.accept();
        if (!channel) {
            continue;  // stop() or a transient accept error
        }
        kvm::log::info("video channel from {}", channel->peer());
        slot.set(std::move(channel));
        windows.set_video(slot.get());  // window frames go both ways on the newest connection
    }
}

// Synthetic stream for --test-video. Stops when `stop` is requested.
void send_test_video(std::stop_token stop, const VideoSlot& slot, const Options& opt) {
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(opt.test_video_kb) * 1024);
    std::mt19937 rng(42);  // random bytes: incompressible, like real JPEG data
    for (auto& b : payload) {
        b = static_cast<std::uint8_t>(rng());
    }
    const auto period = std::chrono::microseconds(1'000'000 / std::max(1, opt.test_video_fps));
    kvm::log::info("test video: {} KB frames at {} fps", opt.test_video_kb, opt.test_video_fps);

    std::uint32_t seq = 0;
    auto next = std::chrono::steady_clock::now();
    while (!stop.stop_requested()) {
        if (const auto channel = slot.get(); channel && channel->is_open()) {
            channel->send_frame({.seq = seq++, .t_capture_us = kvm::steady_now_us(), .width = 1920, .height = 1080,
                                 .codec = "test"},
                                payload);
        }
        next += period;
        const auto now = std::chrono::steady_clock::now();
        if (next < now) {
            next = now;  // network is slower than the frame rate: send as fast as it allows, no backlog
        }
        std::this_thread::sleep_until(next);
    }
}

std::string narrow(std::wstring_view s) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

// A's answer to the client's hello: A's primary monitor, so B can size its virtual display to it.
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

// Waits for the client's hello and checks the protocol version. nullopt = reject the client.
std::optional<proto::Hello> handshake(kvm::net::Connection& conn) {
    const auto first = conn.incoming().pop_for(kHelloTimeout);
    if (!first) {
        kvm::log::warn("{}: no hello within {} s, dropping", conn.peer(), kHelloTimeout.count());
        return std::nullopt;
    }
    const auto* hello = std::get_if<proto::Hello>(&*first);
    if (!hello) {
        kvm::log::warn("{}: expected hello, got {}, dropping", conn.peer(), proto::type_of(*first));
        return std::nullopt;
    }
    if (hello->protocol_version != proto::kProtocolVersion) {
        kvm::log::error("{}: protocol v{} but this host speaks v{} — rebuild both sides from the same commit",
                        conn.peer(), hello->protocol_version, proto::kProtocolVersion);
        return std::nullopt;
    }
    return *hello;
}

// Windows may put the VDD back on at the driver's 800x600: give it the other PC's real size, on the
// other PC's side, so the cursor crosses a same-sized screen instead of catching on a tiny one.
void match_vdd_to_peer(int width, int height, kvm::display::Side side) {
    if (auto vdd = kvm::VirtualDisplay::find(); vdd && vdd->attached()) vdd->match_peer(width, height, side);
}

void run_session(kvm::net::Connection& conn, kvm::KvmLink* kvm, kvm::clip::ClipboardSync* clip, VideoSlot& video,
                 const Options& opt, kvm::display::Side client_side, kvm::WindowLink& windows) {
    const auto hello = handshake(conn);
    if (!hello) {
        return;
    }
    kvm::log::info("client '{}' connected from {}: screen {}x{}, dpi scale {:.2f}", hello->hostname, conn.peer(),
                   hello->screen_w, hello->screen_h, hello->dpi_scale);
    conn.send(make_hello(opt.desk));
    if (!opt.desk) match_vdd_to_peer(hello->screen_w, hello->screen_h, client_side);  // desk mode sizes it itself

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
    windows.attach([&conn](const proto::Message& m) { return conn.send_json(proto::encode(m)); }, hello->screen_w,
                   hello->screen_h);
    struct WindowsDetach {  // a window sent to B comes home before conn goes away
        kvm::WindowLink& windows;
        ~WindowsDetach() { windows.detach(); }
    } windows_detach{windows};
    windows.set_peer(*hello);  // desk mode: B's screen becomes one more monitor of A

    std::jthread test_video;
    if (opt.test_video) {
        test_video = std::jthread([&](std::stop_token stop) { send_test_video(stop, video, opt); });
    }

    auto next_stats = std::chrono::steady_clock::now() + kStatsInterval;
    while (g_running && conn.is_open()) {
        if (const auto msg = conn.incoming().pop_for(500ms)) {
            if (kvm && kvm->handle(*msg)) {
                // KVM: B's cursor came back, or B controls A
            } else if (windows.handle(*msg)) {
                // windows: B's viewer controls A's window (remote_input), or B's window streams here
            } else if (const auto* c = std::get_if<proto::Clipboard>(&*msg); c && clip) {
                clip->receive(to_chunk(*c));
            } else {
                kvm::log::info("from client: {}", proto::encode(*msg).dump());
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
    // The video channel is left alone: a reconnecting client may already have opened its new one
    // (accepted on another thread), and closing here would kill that. A newer channel replaces it.
    kvm::log::info("client '{}' session ended", hello->hostname);
}

Options parse_args(int argc, wchar_t** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == L"--config" && has_value) {
            opt.config_path = argv[++i];
        } else if (arg == L"--log" && has_value) {
            kvm::log::open_file(argv[++i]);
        } else if (arg == L"--no-kvm") {
            opt.kvm_enabled = false;
        } else if (arg == L"--no-clipboard") {
            opt.clipboard = false;
        } else if (arg == L"--no-desk") {
            opt.desk = false;
        } else if (arg == L"--test-video") {
            opt.test_video = true;
        } else if (arg == L"--test-video-kb" && has_value) {
            opt.test_video_kb = std::max(1, _wtoi(argv[++i]));
        } else if (arg == L"--test-video-fps" && has_value) {
            opt.test_video_fps = std::max(1, _wtoi(argv[++i]));
        }
    }
    return opt;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
    Options opt = parse_args(argc, argv);

    try {
        struct CleanedUp {  // first in this scope, so it is destroyed last
            ~CleanedUp() { g_cleaned_up = true; }
        } cleaned_up;
        const kvm::Config cfg = kvm::load_config(opt.config_path);
        opt.desk = opt.desk && cfg.desk;
        g_desk = opt.desk;

        // KVM: hooks run for the whole process; sessions attach/detach the client.
        const auto side = kvm::display::parse_side(cfg.client_side);
        if (opt.kvm_enabled && !side) {
            kvm::log::error("config: client_side must be \"left\", \"right\", \"top\" or \"bottom\", got \"{}\" - KVM disabled",
                            cfg.client_side);
            opt.kvm_enabled = false;
        }
        // Desk mode (Temo): A's virtual display = B's screen; the KVM hooks and WindowLink share it.
        kvm::desk::Region desk_region;
        kvm::desk::Region* desk = opt.desk ? &desk_region : nullptr;
        std::optional<kvm::KvmLink> kvm_link;
        if (opt.kvm_enabled) {
            kvm_link.emplace(kvm::KvmOptions{.peer_side = *side, .is_host = true, .desk = desk});
            kvm_link->start();  // false = hooks failed (logged); B can still control A
        }
        kvm::clip::ClipboardSync clip;
        const bool clip_on = opt.clipboard && clip.start();

        kvm::net::Server server(cfg.control_port);
        kvm::net::VideoServer video_server(cfg.video_port);
        kvm::files::FileServer file_server(cfg.file_port);
        g_server = &server;
        g_video_server = &video_server;
        g_file_server = &file_server;
        kvm::log::info("kvm_host: protocol v{}, listening on ports {} + {} (client on the {}). Ctrl+C to quit.",
                       proto::kProtocolVersion, cfg.control_port, cfg.video_port, cfg.client_side);

        // Lets the other PC find this one by name on the local network (no IP to type there).
        std::optional<kvm::discovery::Responder> discovery;
        try {
            const std::string name = make_hello(false).hostname;
            discovery.emplace(name, cfg.control_port);
            kvm::log::info("discovery: the other PC can find '{}' on the local network (UDP {})", name,
                           kvm::discovery::kDiscoveryPort);
        } catch (const std::exception& e) {
            kvm::log::warn("{} - the other PC needs this PC's IP (--host)", e.what());
        }

        // Windows A <-> B (Ctrl+Alt+Shift+W): a window of A goes to B's viewer and back; B's windows
        // show up in the viewer here.
        kvm::WindowLink windows({.jpeg_quality = cfg.jpeg_quality,
                                 .max_fps = cfg.max_fps,
                                 .peer_side = side.value_or(kvm::display::Side::Right),
                                 .desk = desk});
        // Ctrl+Alt+Shift+Q also ends desk mode (the hooks that keep the cursor off the VDD are gone).
        if (kvm_link) kvm_link->set_on_emergency([&windows] { windows.disable_desk(); });
        struct EmergencyReset {  // declared after `windows`, so it runs before `windows` goes away
            kvm::KvmLink* link;
            ~EmergencyReset() {
                if (link) link->set_on_emergency(nullptr);
            }
        } emergency_reset{kvm_link ? &*kvm_link : nullptr};
        VideoSlot video;
        std::jthread video_acceptor([&] { accept_video_loop(video_server, video, windows); });

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
        }
        // Unhook before `files` goes away (declared after it, so destroyed first).
        struct DragReset {
            kvm::KvmLink* link;
            ~DragReset() {
                if (link) link->set_on_drag_at_edge(nullptr);
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
        std::jthread file_acceptor([&] {
            while (g_running) {
                if (auto channel = file_server.accept(files.callbacks())) {
                    kvm::log::info("file channel from {}", channel->peer());
                    files.set(std::move(channel));
                }
            }
        });
        kvm::Hotkeys hotkeys;
        hotkeys.add('F', [&] { files.show_catcher_at_cursor(); });
        hotkeys.add('W', [&] { windows.toggle(); });
        if (hotkeys.start()) {
            kvm::log::info("Ctrl+Alt+Shift+F: drop files on the box at the cursor to send them to the other PC");
            kvm::log::info("Ctrl+Alt+Shift+W: send the window in front to the other PC, press again to bring it back");
        }

        while (g_running) {
            auto conn = server.accept();
            if (!conn) {
                continue;  // stop() or a transient accept error
            }
            run_session(*conn, kvm_link ? &*kvm_link : nullptr, clip_on ? &clip : nullptr, video, opt,
                        side.value_or(kvm::display::Side::Right), windows);
        }
        hotkeys.stop();
        file_server.stop();
        file_acceptor.join();
        video_server.stop();
        video_acceptor.join();
        g_server = nullptr;
        g_video_server = nullptr;
        g_file_server = nullptr;
        kvm::log::info("kvm_host: bye");
        return 0;
    } catch (const std::exception& e) {
        kvm::log::error("{}", e.what());
        return 1;
    }
}
