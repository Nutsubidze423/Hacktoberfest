// kvm_host.exe — runs on A (owns the physical mouse/keyboard, streams a window).
// Usage: kvm_host.exe [--config <path>] [--log <file>] [--no-kvm]
//                     [--test-video [--test-video-kb N] [--test-video-fps N]]
//   --no-kvm      don't install the input hooks (e.g. when only testing the video stream)
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

#include "input_capture.h"
#include "kvm/config.h"
#include "kvm/dpi.h"
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
    bool test_video = false;
    int test_video_kb = 250;
    int test_video_fps = 30;
};

std::atomic<bool> g_running{true};
std::atomic<kvm::net::Server*> g_server{nullptr};
std::atomic<kvm::net::VideoServer*> g_video_server{nullptr};

BOOL WINAPI on_console_ctrl(DWORD) {
    g_running = false;
    if (auto* server = g_server.load()) {
        server->stop();
    }
    if (auto* video = g_video_server.load()) {
        video->stop();
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

void run_session(kvm::net::Connection& conn, kvm::host::InputCapture* capture, VideoSlot& video,
                 const Options& opt) {
    const auto hello = handshake(conn);
    if (!hello) {
        return;
    }
    kvm::log::info("client '{}' connected from {}: screen {}x{}, dpi scale {:.2f}", hello->hostname, conn.peer(),
                   hello->screen_w, hello->screen_h, hello->dpi_scale);
    conn.send(make_hello());

    // KVM: input goes to this client while the session lasts. Detaching blocks until the
    // capture stops using conn, so it must happen before conn goes away (struct dtor).
    struct Detach {
        kvm::host::InputCapture* capture;
        ~Detach() {
            if (capture) capture->set_client(nullptr);
        }
    } detach{capture};
    if (capture) {
        capture->set_client([&conn](const proto::Message& m) { return conn.send_json(proto::encode(m)); });
    }

    std::jthread test_video;
    if (opt.test_video) {
        test_video = std::jthread([&](std::stop_token stop) { send_test_video(stop, video, opt); });
    }

    auto next_stats = std::chrono::steady_clock::now() + kStatsInterval;
    while (g_running && conn.is_open()) {
        if (const auto msg = conn.incoming().pop_for(500ms)) {
            if (const auto* leave = std::get_if<proto::FocusLeave>(&*msg); leave && capture) {
                capture->on_focus_leave(*leave);  // B's cursor came back through its edge
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
        std::optional<kvm::host::InputCapture> capture;
        if (opt.kvm_enabled) {
            capture.emplace(kvm::host::CaptureOptions{.client_side = *side});
            if (!capture->start()) {
                kvm::log::error("KVM disabled: could not install the input hooks");
                capture.reset();
            }
        }

        kvm::net::Server server(cfg.control_port);
        kvm::net::VideoServer video_server(cfg.video_port);
        g_server = &server;
        g_video_server = &video_server;
        kvm::log::info("kvm_host: protocol v{}, listening on ports {} + {} (client on the {}). Ctrl+C to quit.",
                       proto::kProtocolVersion, cfg.control_port, cfg.video_port, cfg.client_side);

        VideoSlot video;
        std::jthread video_acceptor([&] { accept_video_loop(video_server, video); });

        while (g_running) {
            auto conn = server.accept();
            if (!conn) {
                continue;  // stop() or a transient accept error
            }
            run_session(*conn, capture ? &*capture : nullptr, video, opt);
        }
        video_server.stop();
        video_acceptor.join();
        g_server = nullptr;
        g_video_server = nullptr;
        kvm::log::info("kvm_host: bye");
        return 0;
    } catch (const std::exception& e) {
        kvm::log::error("{}", e.what());
        return 1;
    }
}
