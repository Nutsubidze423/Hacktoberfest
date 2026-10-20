// video_sender_test: VideoSender without a network (T7). A producer puts 60 frames/s into a
// LatestSlot, like WGC does; a fake channel spends a fixed time per frame, like Wi-Fi does.
//
// Scenarios: a normal channel (frame rate must be capped at max_fps), a slow channel (old frames
// must be skipped, not queued, so latency stays flat), a channel that closes (the sender must stop
// by itself), and Luka's real FrameChannel over TCP on 127.0.0.1 (B receives with recv_frame).
// Every payload must be a valid JPEG with gap-free seq numbers.
//
//   video_sender_test.exe

#include <turbojpeg.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "kvm/log.h"
#include "kvm/net.h"
#include "jpeg_encoder.h"
#include "kvm/video.h"
#include "video_sender.h"

namespace {

using namespace std::chrono_literals;

struct Checks {
    int failed = 0;
    void operator()(bool ok, std::string_view what) {
        kvm::log::info("{} {}", ok ? "[ok]  " : "[FAIL]", what);
        if (!ok) ++failed;
    }
};

// A 1920x1080 screen with a block at a different place in each frame.
std::vector<kvm::CapturedFrame> moving_block_frames() {
    std::vector<kvm::CapturedFrame> frames(3);
    for (size_t i = 0; i < frames.size(); ++i) {
        auto& f = frames[i];
        f.width = 1920;
        f.height = 1080;
        f.stride = f.width * 4;
        f.bgra.assign(static_cast<size_t>(f.stride) * f.height, 230);
        for (int y = 400; y < 600; ++y) {
            for (int x = 300 + static_cast<int>(i) * 500; x < 600 + static_cast<int>(i) * 500; ++x) {
                auto* p = f.bgra.data() + static_cast<size_t>(y) * f.stride + static_cast<size_t>(x) * 4;
                p[0] = 200, p[1] = 80, p[2] = 30;
            }
        }
    }
    return frames;
}

// The most frames/s this CPU can send through a channel that takes `send_ms` per frame: the sender
// encodes and sends one frame after the other, so a slow CPU (a Celeron N4020 encodes 1080p in about
// 20 ms) can't reach max_fps however good the pacing is.
double reachable_fps(double send_ms, int max_fps) {
    const auto pattern = moving_block_frames();
    kvm::JpegEncoder jpeg(75);
    std::vector<std::uint8_t> out;
    constexpr int kRuns = 10;
    double total_ms = 0;
    for (int i = 0; i < kRuns; ++i) {
        jpeg.encode(pattern[i % pattern.size()], out);
        total_ms += jpeg.last_encode_ms();
    }
    return std::min(static_cast<double>(max_fps), 1000.0 / (total_ms / kRuns + send_ms));
}

struct Result {
    std::vector<kvm::proto::FrameHeader> headers;
    std::vector<std::int64_t> latency_us;  // capture -> fully sent
    bool all_valid_jpeg = true;
    double seconds = 0;
    std::uint64_t skipped = 0;
    double stop_ms = 0;
    bool stopped_by_itself = false;
};

Result run(std::chrono::milliseconds send_time, int max_fps, std::chrono::milliseconds duration, int close_after = -1) {
    static const auto pattern = moving_block_frames();
    kvm::LatestSlot<kvm::CapturedFrame> slot;
    std::atomic<bool> producing = true;
    std::thread producer([&] {
        for (size_t i = 0; producing; ++i) {
            auto f = pattern[i % pattern.size()];
            f.t_capture_us = kvm::steady_now_us();
            slot.put(std::move(f));
            std::this_thread::sleep_for(16ms);
        }
    });

    Result r;
    std::mutex mutex;
    tjhandle decoder = tj3Init(TJINIT_DECOMPRESS);  // only used on the sender thread
    int calls = 0;
    auto fake_send = [&](const kvm::proto::FrameHeader& h, std::span<const std::uint8_t> payload) {
        if (close_after >= 0 && calls++ >= close_after) return false;
        const bool valid = tj3DecompressHeader(decoder, payload.data(), payload.size()) == 0 &&
                           tj3Get(decoder, TJPARAM_JPEGWIDTH) == h.width &&
                           tj3Get(decoder, TJPARAM_JPEGHEIGHT) == h.height;
        std::this_thread::sleep_for(send_time);
        std::lock_guard lock(mutex);
        r.headers.push_back(h);
        r.latency_us.push_back(kvm::steady_now_us() - h.t_capture_us);
        r.all_valid_jpeg = r.all_valid_jpeg && valid;
        return true;
    };

    const auto start = std::chrono::steady_clock::now();
    {
        kvm::VideoSender sender(slot, fake_send, {.jpeg_quality = 75, .max_fps = max_fps});
        std::this_thread::sleep_for(duration);
        r.stopped_by_itself = !sender.running();
        const auto stop_start = std::chrono::steady_clock::now();
        sender.stop();
        r.stop_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stop_start).count();
    }
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    producing = false;
    producer.join();
    tj3Destroy(decoder);
    r.skipped = slot.overwritten();
    return r;
}

// Listens on 127.0.0.1 only (a free port), so Windows Firewall doesn't ask anything.
SOCKET listen_loopback(std::uint16_t& port) {
    kvm::net::ensure_winsock();
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof(addr);
    if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 1) != 0 ||
        getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        if (s != INVALID_SOCKET) closesocket(s);
        return INVALID_SOCKET;
    }
    port = ntohs(addr.sin_port);
    return s;
}

// A: VideoSender -> FrameChannel::send_frame. B: connect_video -> recv_frame, like kvm_client.
Result run_over_tcp(std::chrono::milliseconds duration) {
    static const auto pattern = moving_block_frames();
    Result r;
    std::uint16_t port = 0;
    const SOCKET listener = listen_loopback(port);
    if (listener == INVALID_SOCKET) return r;
    std::unique_ptr<kvm::net::FrameChannel> a_side;
    std::thread acceptor([&] {
        const SOCKET s = accept(listener, nullptr, nullptr);
        if (s != INVALID_SOCKET) a_side = std::make_unique<kvm::net::FrameChannel>(s, "test B");
    });
    auto b_side = kvm::net::connect_video("127.0.0.1", port);
    acceptor.join();
    closesocket(listener);
    if (!a_side || !b_side) return r;

    std::thread receiver([&] {
        tjhandle decoder = tj3Init(TJINIT_DECOMPRESS);
        while (auto frame = b_side->recv_frame()) {
            const bool valid = tj3DecompressHeader(decoder, frame->payload.data(), frame->payload.size()) == 0 &&
                               tj3Get(decoder, TJPARAM_JPEGWIDTH) == frame->header.width &&
                               tj3Get(decoder, TJPARAM_JPEGHEIGHT) == frame->header.height;
            r.all_valid_jpeg = r.all_valid_jpeg && valid;
            r.latency_us.push_back(kvm::steady_now_us() - frame->header.t_capture_us);
            r.headers.push_back(frame->header);
        }
        tj3Destroy(decoder);
    });

    kvm::LatestSlot<kvm::CapturedFrame> slot;
    std::atomic<bool> producing = true;
    std::thread producer([&] {
        for (size_t i = 0; producing; ++i) {
            auto f = pattern[i % pattern.size()];
            f.t_capture_us = kvm::steady_now_us();
            slot.put(std::move(f));
            std::this_thread::sleep_for(16ms);
        }
    });
    const auto start = std::chrono::steady_clock::now();
    {
        kvm::VideoSender sender(slot, *a_side, {.jpeg_quality = 75, .max_fps = 30});
        std::this_thread::sleep_for(duration);
    }
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    a_side->close();  // B's recv_frame() returns nullopt
    receiver.join();
    b_side->close();
    producing = false;
    producer.join();
    return r;
}

bool seq_without_gaps(const Result& r) {
    for (size_t i = 0; i < r.headers.size(); ++i) {
        if (r.headers[i].seq != i || r.headers[i].codec != "jpeg") return false;
    }
    return true;
}

double average_ms(const std::vector<std::int64_t>& us, size_t from, size_t to) {
    if (from >= to) return 0;
    std::int64_t sum = 0;
    for (size_t i = from; i < to; ++i) sum += us[i];
    return static_cast<double>(sum) / static_cast<double>(to - from) / 1000.0;
}

}  // namespace

int main() {
    Checks check;

    kvm::log::info("1) channel takes 20 ms per frame, max_fps 30, producer 60 fps");
    {
        const double reachable = reachable_fps(20.0, 30);
        const auto r = run(20ms, 30, 3s);
        const double fps = static_cast<double>(r.headers.size()) / r.seconds;
        check(!r.headers.empty() && seq_without_gaps(r), std::format("{} frames, seq 0..n-1, codec jpeg", r.headers.size()));
        check(r.all_valid_jpeg, "every payload is a 1920x1080 JPEG");
        // The floor leaves room for the producer thread and the rest of the machine sharing the CPU.
        check(fps <= 31.0 && fps >= 0.6 * reachable,
              std::format("{:.1f} frames/s (cap 30; this CPU reaches about {:.0f})", fps, reachable));
        check(r.skipped > 0, std::format("{} older frames skipped instead of queued", r.skipped));
        check(r.stop_ms < 300, std::format("stop() took {:.0f} ms", r.stop_ms));
        kvm::log::info("   capture -> sent: {:.1f} ms on average", average_ms(r.latency_us, 0, r.latency_us.size()));
    }

    kvm::log::info("2) slow channel: 100 ms per frame");
    {
        const auto r = run(100ms, 30, 3s);
        const size_t n = r.latency_us.size();
        const double early = average_ms(r.latency_us, 0, n / 2), late = average_ms(r.latency_us, n / 2, n);
        check(n >= 10 && seq_without_gaps(r), std::format("{} frames, seq without gaps", n));
        check(late < early + 50.0, std::format("latency stays flat: {:.0f} ms early, {:.0f} ms late", early, late));
    }

    kvm::log::info("3) channel closes after 5 frames");
    {
        const auto r = run(5ms, 30, 1s, 5);
        check(r.stopped_by_itself, "sender stopped by itself");
        check(r.headers.size() == 5, std::format("{} frames sent before the close", r.headers.size()));
    }

    kvm::log::info("4) Luka's FrameChannel over TCP on 127.0.0.1, max_fps 30");
    {
        const auto r = run_over_tcp(2s);
        const double fps = static_cast<double>(r.headers.size()) / r.seconds;
        check(r.headers.size() >= 20 && seq_without_gaps(r),
              std::format("B received {} frames, seq 0..n-1, codec jpeg", r.headers.size()));
        check(r.all_valid_jpeg, "every received payload is a 1920x1080 JPEG");
        check(fps <= 31.0, std::format("{:.1f} frames/s (cap 30)", fps));
        kvm::log::info("   capture -> received on B: {:.1f} ms on average",
                       average_ms(r.latency_us, 0, r.latency_us.size()));
    }

    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
