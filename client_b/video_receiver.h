// B side of the video channel (port 50501): connect to A, receive JPEG frames, decode, show.
//
//   kvm::ViewerWindow viewer;  viewer.create(...);
//   kvm::VideoReceiver receiver(cfg.host_ip, cfg.video_port, viewer);   // runs until destroyed
//   receiver.set_clock_offset_us(conn->stats().clock_offset_us);        // optional, for latency
//
// Threads: the receive thread reads frames from the socket into a LatestSlot; the decode thread
// takes the newest one, decodes it and presents it. If decoding falls behind, older frames are
// overwritten (dropped) instead of queueing up, so the picture never lags behind.
#pragma once

#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "kvm/protocol.h"
#include "kvm/queue.h"
#include "viewer_window.h"

namespace kvm {

// TEMPORARY: minimal framing for port 50501 until Luka's send_frame/recv_frame exist in net.*.
// Wire format (CLAUDE.md): [u32 header_len][u32 payload_len][header JSON][payload], big-endian.
// When Luka's version lands, switch to it and delete this namespace.
namespace video_wire {

struct Frame {
    proto::FrameHeader header;
    std::vector<std::uint8_t> payload;
};

// Owns a SOCKET and closes it on destruction. close() may be called from another thread to
// unblock a recv()/accept() in progress.
class Socket {
public:
    Socket() = default;
    explicit Socket(SOCKET s) : sock_(s) {}
    ~Socket() { close(); }
    Socket(Socket&& other) noexcept : sock_(other.sock_.exchange(INVALID_SOCKET)) {}
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    SOCKET get() const { return sock_; }
    bool valid() const { return sock_ != INVALID_SOCKET; }
    void close();

private:
    std::atomic<SOCKET> sock_{INVALID_SOCKET};
};

// Listening socket on 0.0.0.0:port. Throws std::runtime_error on failure.
Socket listen_tcp(std::uint16_t port);
// Connected socket with TCP_NODELAY, or an invalid Socket after `timeout`. *error gets the WSA code.
Socket connect_tcp(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout, int* error);

bool send_frame(SOCKET s, const proto::FrameHeader& header, std::span<const std::uint8_t> payload);
// 0 on success, otherwise a WSA error code (WSAECONNRESET when the peer closed, WSAEMSGSIZE /
// WSAEINVAL on malformed framing).
int recv_frame(SOCKET s, Frame& out);

}  // namespace video_wire

class VideoReceiver {
public:
    // Starts connecting right away; reconnects every second while A is not reachable.
    VideoReceiver(std::string host, std::uint16_t port, ViewerWindow& viewer);
    ~VideoReceiver();

    VideoReceiver(const VideoReceiver&) = delete;
    VideoReceiver& operator=(const VideoReceiver&) = delete;

    // A's clock minus B's clock (net::LinkStats::clock_offset_us). Until this is called the viewer
    // shows latency as unknown; 0 is exact when A and B are the same machine.
    void set_clock_offset_us(std::int64_t offset) {
        clock_offset_us_ = offset;
        has_clock_offset_ = true;
    }

private:
    void receive_loop(std::stop_token stop);
    void decode_loop(std::stop_token stop);

    std::string host_;
    std::uint16_t port_;
    ViewerWindow& viewer_;
    std::atomic<std::int64_t> clock_offset_us_{0};
    std::atomic<bool> has_clock_offset_{false};

    video_wire::Socket sock_;  // closed from the destructor to unblock recv()
    LatestSlot<video_wire::Frame> latest_;

    // Declared last: joined first on destruction, while everything above is still alive.
    std::jthread receive_thread_;
    std::jthread decode_thread_;
};

}  // namespace kvm
