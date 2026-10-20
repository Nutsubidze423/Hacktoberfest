// SPDX-License-Identifier: Apache-2.0

// Loopback check for kvm/video.h: frames of many sizes arrive intact and in order, concurrent
// senders do not interleave bytes, close() unblocks a receiver, garbage input is rejected, and a
// rough throughput number for 1080p-sized JPEG frames.
// Usage: video_test.exe   (exit code 0 = all passed; a free TCP port on 127.0.0.1, picked by Windows)

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <thread>
#include <vector>

#include "kvm/net.h"
#include "kvm/video.h"

using namespace std::chrono_literals;
namespace net = kvm::net;
namespace proto = kvm::proto;

namespace {

// Picked by Windows at start: a fixed port can fall in a range Hyper-V/WSL reserve (WSAEACCES).
std::uint16_t g_port = 0;

std::uint16_t free_port() {
    net::ensure_winsock();
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    int len = sizeof(addr);
    std::uint16_t port = 0;
    if (s != INVALID_SOCKET && ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
        ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        port = ntohs(addr.sin_port);
    }
    if (s != INVALID_SOCKET) closesocket(s);
    return port;
}
int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

// Deterministic payload so the receiver can verify every byte without keeping a copy.
std::vector<std::uint8_t> make_payload(std::uint32_t seq, std::size_t size) {
    std::vector<std::uint8_t> p(size);
    std::uint32_t x = seq * 2654435761u + 1;
    for (auto& b : p) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b = static_cast<std::uint8_t>(x);
    }
    return p;
}

std::size_t size_for(std::uint32_t seq) {
    static constexpr std::size_t kSizes[] = {0, 1, 7, 1000, 65'536, 300'000, 1'000'003, 2'500'000};
    return kSizes[seq % std::size(kSizes)];
}

struct Pair {
    std::unique_ptr<net::FrameChannel> server_side;
    std::unique_ptr<net::FrameChannel> client_side;
};

Pair connect_pair(net::VideoServer& server) {
    Pair pair;
    std::thread acceptor([&] { pair.server_side = server.accept(); });
    pair.client_side = net::connect_video("127.0.0.1", g_port);
    acceptor.join();
    return pair;
}

void test_sizes_and_order(net::VideoServer& server) {
    auto [a, b] = connect_pair(server);
    check(a && b, "connect: VideoServer::accept + connect_video");
    if (!a || !b) {
        return;
    }
    constexpr std::uint32_t kFrames = 64;
    std::thread sender([&] {
        for (std::uint32_t seq = 0; seq < kFrames; ++seq) {
            const auto payload = make_payload(seq, size_for(seq));
            a->send_frame({.seq = seq, .t_capture_us = kvm::steady_now_us(), .width = 1920, .height = 1080}, payload);
        }
    });
    bool intact = true;
    std::uint32_t received = 0;
    for (std::uint32_t seq = 0; seq < kFrames; ++seq) {
        const auto frame = b->recv_frame();
        if (!frame) {
            break;
        }
        intact = intact && frame->header.seq == seq && frame->header.width == 1920 && frame->header.codec == "jpeg" &&
                 frame->payload == make_payload(seq, size_for(seq));
        ++received;
    }
    sender.join();
    check(received == kFrames && intact, "64 frames, 0 B .. 2.5 MB: every byte, header and order intact");
}

void test_concurrent_senders(net::VideoServer& server) {
    auto [a, b] = connect_pair(server);
    if (!a || !b) {
        check(false, "concurrent: connect");
        return;
    }
    constexpr std::uint32_t kPerThread = 40;
    auto send_range = [&](std::uint32_t first) {
        for (std::uint32_t seq = first; seq < first + kPerThread; ++seq) {
            a->send_frame({.seq = seq}, make_payload(seq, 100'000 + seq * 997));
        }
    };
    std::thread t1(send_range, 0);
    std::thread t2(send_range, 1000);
    bool intact = true;
    for (std::uint32_t i = 0; i < 2 * kPerThread; ++i) {
        const auto frame = b->recv_frame();
        intact =
            intact && frame && frame->payload == make_payload(frame->header.seq, 100'000 + frame->header.seq * 997);
    }
    t1.join();
    t2.join();
    check(intact, "2 sender threads: frames never interleave");
}

void test_close_unblocks(net::VideoServer& server) {
    auto [a, b] = connect_pair(server);
    if (!a || !b) {
        check(false, "close: connect");
        return;
    }
    std::atomic<bool> got_nullopt{false};
    std::thread receiver([&] { got_nullopt = !b->recv_frame().has_value(); });
    std::this_thread::sleep_for(50ms);
    b->close();
    receiver.join();
    check(got_nullopt && !b->is_open(), "close() from another thread unblocks recv_frame");

    // The peer sees the close too.
    check(!a->recv_frame().has_value(), "peer's recv_frame returns nullopt after close");
    check(!b->send_frame({}, {}), "send_frame after close returns false");
}

void test_garbage_rejected(net::VideoServer& server) {
    std::unique_ptr<net::FrameChannel> server_side;
    std::thread acceptor([&] { server_side = server.accept(); });
    const SOCKET raw = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ::connect(raw, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    acceptor.join();
    const char junk[] = "\xFF\xFF\xFF\xFFGET / HTTP/1.1\r\n\r\n";
    ::send(raw, junk, sizeof(junk) - 1, 0);
    const bool rejected = server_side && !server_side->recv_frame().has_value();
    closesocket(raw);
    check(rejected, "garbage length prefix -> nullopt, channel closed");
}

void test_throughput(net::VideoServer& server) {
    auto [a, b] = connect_pair(server);
    if (!a || !b) {
        check(false, "throughput: connect");
        return;
    }
    constexpr int kFrames = 300;
    const auto payload = make_payload(1, 250'000);  // typical 1080p JPEG at quality ~75
    const auto start = std::chrono::steady_clock::now();
    std::thread sender([&] {
        for (int i = 0; i < kFrames; ++i) {
            a->send_frame({.seq = static_cast<std::uint32_t>(i)}, payload);
        }
    });
    int received = 0;
    while (received < kFrames && b->recv_frame()) {
        ++received;
    }
    sender.join();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("       loopback: %d x 250 KB in %.2f s = %.0f frames/s, %.0f MB/s\n", received, seconds,
                received / seconds, received * 0.25 / seconds);
    check(received == kFrames, "throughput run completed");
}

}  // namespace

int main() {
    try {
        g_port = free_port();
        net::VideoServer server(g_port);
        test_sizes_and_order(server);
        test_concurrent_senders(server);
        test_close_unblocks(server);
        test_garbage_rejected(server);
        test_throughput(server);
    } catch (const std::exception& e) {
        std::printf("[FAIL] %s\n", e.what());
        ++g_failures;
    }
    std::printf("\n%s\n", g_failures == 0 ? "all passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
