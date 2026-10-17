// kvm_host.exe — runs on A (owns the physical mouse/keyboard, streams a window).
// Usage: kvm_host.exe [--config <path>] [--log <file>] [--no-kvm] [--no-clipboard]
//                     [--test-video [--test-video-kb N] [--test-video-fps N]]
//   --no-kvm      don't install the input hooks (e.g. when only testing the video stream)
//   --no-clipboard  don't share copied text/images with the client
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
#include "kvm/dpi.h"
#include "kvm/file_link.h"
#include "kvm/file_transfer.h"
#include "kvm/hotkeys.h"
#include "kvm/kvm_link.h"
#include "kvm/log.h"
#include "kvm/net.h"
#include "kvm/protocol.h"
#include "kvm/video.h"

namespace {

using namespace std::chrono_literals;
namespace proto = kvm::proto;

constexpr auto kHelloTimeout = 3s;
constexpr auto kStatsInterval = 5s;

struct Options {
    std::optional<std::filesystem::path> config_path;
    bool kvm_enabled = true;
    bool clipboard = true;
    bool test_video = false;
    int test_video_kb = 250;
    int test_video_fps = 30;
};

std::atomic<bool> g_running{true};
std::atomic<kvm::net::Server*> g_server{nullptr};
std::atomic<kvm::net::VideoServer*> g_video_server{nullptr};
std::atomic<kvm::files::FileServer*> g_file_server{nullptr};

BOOL WINAPI on_console_ctrl(DWORD) {
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

void accept_video_loop(kvm::net::VideoServer& server, VideoSlot& slot) {
    while (g_running) {
        auto channel = server.accept();
        if (!channel) {
            continue;  // stop() or a transient accept error
        }
        kvm::log::info("video channel from {}", channel->peer());
        slot.set(std::move(channel));
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

void run_session(kvm::net::Connection& conn, kvm::KvmLink* kvm, kvm::clip::ClipboardSync* clip, VideoSlot& video,
                 const Options& opt) {
    const auto hello = handshake(conn);
    if (!hello) {
        return;
    }
    kvm::log::info("client '{}' connected from {}: screen {}x{}, dpi scale {:.2f}", hello->hostname, conn.peer(),
                   hello->screen_w, hello->screen_h, hello->dpi_scale);
    conn.send(make_hello());

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

    std::jthread test_video;
    if (opt.test_video) {
        test_video = std::jthread([&](std::stop_token stop) { send_test_video(stop, video, opt); });
    }

    auto next_stats = std::chrono::steady_clock::now() + kStatsInterval;
    while (g_running && conn.is_open()) {
        if (const auto msg = conn.incoming().pop_for(500ms)) {
            if (kvm && kvm->handle(*msg)) {
                // KVM: B's cursor came back, or B controls A
            } else if (const auto* c = std::get_if<proto::Clipboard>(&*msg); c && clip) {
                clip->receive(to_chunk(*c));
            } else {
                // TODO(luka): route to remote_input (sandro) once it exists.
                kvm::log::info("from client: {}", proto::encode(*msg).dump());
            }
        }
        if (std::chrono::steady_clock::now() >= next_stats) {
            const auto stats = conn.stats();
            kvm::log::info("link: rtt {:.2f} ms, clock offset {:.1f} ms", stats.rtt_us / 1000.0,
                           stats.clock_offset_us / 1000.0);
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
        const kvm::Config cfg = kvm::load_config(opt.config_path);

        // KVM: hooks run for the whole process; sessions attach/detach the client.
        const auto side = kvm::display::parse_side(cfg.client_side);
        if (opt.kvm_enabled && (!side || (*side != kvm::display::Side::Left && *side != kvm::display::Side::Right))) {
            kvm::log::error("config: client_side must be \"left\" or \"right\", got \"{}\" - KVM disabled", cfg.client_side);
            opt.kvm_enabled = false;
        }
        std::optional<kvm::KvmLink> kvm_link;
        if (opt.kvm_enabled) {
            kvm_link.emplace(kvm::KvmOptions{.peer_side = *side, .is_host = true});
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

        VideoSlot video;
        std::jthread video_acceptor([&] { accept_video_loop(video_server, video); });

        // Files (port 50502): drop window + Ctrl+Alt+Shift+F, received files land in Downloads\KVM.
        kvm::FileLink files;
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
        if (hotkeys.start()) {
            kvm::log::info("Ctrl+Alt+Shift+F: drop files on the box at the cursor to send them to the other PC");
        }

        while (g_running) {
            auto conn = server.accept();
            if (!conn) {
                continue;  // stop() or a transient accept error
            }
            run_session(*conn, kvm_link ? &*kvm_link : nullptr, clip_on ? &clip : nullptr, video, opt);
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
