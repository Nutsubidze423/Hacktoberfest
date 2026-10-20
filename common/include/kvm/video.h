// SPDX-License-Identifier: Apache-2.0

// Video channel (port 50501): one encoded frame per message, over TLS 1.3 like the control channel.
// Wire format: [u32 header_len][u32 payload_len][header JSON (proto::FrameHeader)][payload], big-endian.
// It belongs to the live control session (kvm/net.h side channels, docs/SECURITY.md 6.4).
//
//   Host: kvm::net::VideoServer server(cfg.video_port);
//         auto pending = server.accept(me);                         // TLS done, nothing read
//         kvm::net::admit_side_channel(*pending->stream, session, "video", pending->peer, [&] {
//             slot.set(std::make_shared<kvm::net::FrameChannel>(std::move(*pending)));  // not past the session
//         });
//         channel->send_frame(header, jpeg_bytes);                  // from the sender thread
//   Client: auto channel = kvm::net::connect_video(ip, port, me, host_id, token);
//         while (auto frame = channel->recv_frame()) { ... }        // from the receiver thread
//
// The channel has no threads, pings or timeouts of its own: an idle stream is normal. Link health
// comes from the control Connection — when it drops, close() the FrameChannel too.
#pragma once

#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "kvm/net.h"
#include "kvm/protocol.h"
#include "kvm/tls.h"

namespace kvm::net {

struct Frame {
    proto::FrameHeader header;          // seq, t_capture_us, width, height, codec
    std::vector<std::uint8_t> payload;  // encoded image (JPEG)
};

class FrameChannel {
public:
    // Takes a TLS connection whose peer has been checked (see above).
    explicit FrameChannel(Handshaken connection);
    // Do not destroy while another thread is inside recv_frame(): close() first, join it, then destroy.
    ~FrameChannel();

    FrameChannel(const FrameChannel&) = delete;
    FrameChannel& operator=(const FrameChannel&) = delete;

    // Blocks until the whole frame is handed to TCP. Thread-safe. False = closed (or just failed).
    bool send_frame(const proto::FrameHeader& header, std::span<const std::uint8_t> payload);

    // Blocks until a frame arrives. Call from one thread only. nullopt = closed or protocol error.
    std::optional<Frame> recv_frame();

    bool is_open() const { return open_; }
    const std::string& peer() const { return peer_; }

    // Unblocks send_frame()/recv_frame() in other threads. Idempotent, any thread.
    void close();

private:
    std::unique_ptr<tls::Stream> tls_;
    std::string peer_;
    std::atomic<bool> open_{true};
    std::mutex send_mutex_;
};

class VideoServer {
public:
    // Listens on 0.0.0.0:port. Throws std::runtime_error if the port cannot be bound.
    explicit VideoServer(std::uint16_t port);
    ~VideoServer();

    VideoServer(const VideoServer&) = delete;
    VideoServer& operator=(const VideoServer&) = delete;

    // Blocks until a client connects and completes the TLS handshake (failures are logged and
    // skipped). nullopt after stop(). Check it with admit_side_channel() before using it.
    std::optional<Handshaken> accept(const tls::Credentials& me);

    // Unblocks accept() from any thread. Idempotent.
    void stop();

private:
    Server server_;
};

// Opens the video side channel of the live session (connect_side_channel). nullptr on failure
// (logged); the caller decides when to retry.
std::unique_ptr<FrameChannel> connect_video(const std::string& host, std::uint16_t port, const tls::Credentials& me,
                                            const tls::DeviceId& host_id, std::string_view token,
                                            std::chrono::milliseconds timeout = std::chrono::seconds(2));

}  // namespace kvm::net
