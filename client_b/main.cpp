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

namespace {

using namespace std::chrono_literals;
namespace proto = kvm::proto;

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

// Counters filled by the video receiver thread, read and reset by the stats log.
struct VideoStats {
    std::atomic<std::int64_t> frames{0};
    std::atomic<std::int64_t> bytes{0};
    std::atomic<std::int64_t> latency_sum_us{0};
    std::atomic<std::int64_t> latency_max_us{0};
    std::atomic<std::int64_t> latency_samples{0};
};

// Receives frames until the channel closes.
// TODO(luka): hand frames to the viewer (sandro) instead of only counting them.
void receive_video(kvm::net::FrameChannel& channel, const kvm::net::Connection& conn, VideoStats& stats) {
    while (const auto frame = channel.recv_frame()) {
        ++stats.frames;
        stats.bytes += static_cast<std::int64_t>(frame->payload.size());
        // Latency needs the clock offset, which exists after the first ping round trip.
        if (conn.stats().rtt_us >= 0) {
            const std::int64_t latency = kvm::steady_now_us() - conn.peer_to_local_us(frame->header.t_capture_us);
            stats.latency_sum_us += latency;
            ++stats.latency_samples;
            std::int64_t prev = stats.latency_max_us;
            while (latency > prev && !stats.latency_max_us.compare_exchange_weak(prev, latency)) {
            }
        }
    }
}

void log_video_stats(VideoStats& stats, std::chrono::seconds interval) {
    const auto frames = stats.frames.exchange(0);
    if (frames == 0) {
        return;
    }
    const auto bytes = stats.bytes.exchange(0);
    const auto samples = stats.latency_samples.exchange(0);
    const auto latency_sum = stats.latency_sum_us.exchange(0);
    const auto latency_max = stats.latency_max_us.exchange(0);
    const double secs = static_cast<double>(interval.count());
    kvm::log::info("video: {:.1f} fps, {:.2f} MB/s ({:.0f} Mbit/s), latency avg {:.1f} ms, max {:.1f} ms",
                   frames / secs, bytes / secs / 1e6, bytes * 8 / secs / 1e6,
                   samples ? latency_sum / 1000.0 / samples : 0.0, latency_max / 1000.0);
}

void run_session(kvm::net::Connection& conn, const kvm::Config& cfg, kvm::KvmLink* kvm,
                 kvm::clip::ClipboardSync* clip, kvm::FileLink& files) {
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

    VideoStats video_stats;
    std::unique_ptr<kvm::net::FrameChannel> video = kvm::net::connect_video(cfg.host_ip, cfg.video_port);
    std::jthread video_receiver;
    if (video) {
        kvm::log::info("video channel connected ({}:{})", cfg.host_ip, cfg.video_port);
        video_receiver = std::jthread([&] { receive_video(*video, conn, video_stats); });
    } else {
        kvm::log::warn("no video channel — is port {} open on the host?", cfg.video_port);
    }
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
            } else if (const auto* c = std::get_if<proto::Clipboard>(&*msg); c && clip) {
                clip->receive(to_chunk(*c));
            } else {
                // TODO(luka): route to viewer (sandro) once it exists.
                kvm::log::info("from host: {}", proto::encode(*msg).dump());
            }
        }
        if (std::chrono::steady_clock::now() >= next_stats) {
            const auto stats = conn.stats();
            kvm::log::info("link: rtt {:.2f} ms, clock offset {:.1f} ms", stats.rtt_us / 1000.0,
                           stats.clock_offset_us / 1000.0);
            log_video_stats(video_stats, std::chrono::duration_cast<std::chrono::seconds>(kStatsInterval));
            next_stats += kStatsInterval;
        }
    }
    files.set(nullptr);
    if (video) {
        video->close();  // unblocks recv_frame; join before `video` is destroyed
        if (video_receiver.joinable()) {
            video_receiver.join();
        }
    }
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
        kvm::Hotkeys hotkeys;
        hotkeys.add('F', [&] { files.show_catcher_at_cursor(); });
        if (hotkeys.start()) {
            kvm::log::info("Ctrl+Alt+Shift+F: drop files on the box at the cursor to send them to the other PC");
        }

        while (g_running) {
            if (auto conn = kvm::net::connect(cfg.host_ip, cfg.control_port)) {
                run_session(*conn, cfg, kvm_link ? &*kvm_link : nullptr, clip_on ? &clip : nullptr, files);
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
