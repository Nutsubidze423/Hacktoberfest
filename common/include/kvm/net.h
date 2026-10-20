// SPDX-License-Identifier: Apache-2.0

// The control channel (port 50500): length-prefixed JSON messages over TLS 1.3 (kvm/tls.h).
//
//   Host:   kvm::net::Server server(port);  auto pending = server.accept(me);
//   Client: auto pending = kvm::net::connect(ip, port, me);  // nullopt on failure, retry yourself
//   // Nothing has been read yet: check pending->stream->peer_id() against the paired PCs
//   // (kvm/trust.h, docs/SECURITY.md 6.4) and only then start reading:
//   kvm::net::Connection conn(std::move(*pending));
//
//   conn.send(proto::MouseMove{.dx = 3, .dy = -1});         // thread-safe, never throws
//   while (auto msg = conn.incoming().pop()) { ... }         // ends when the connection drops
//
// Each Connection runs a receive thread and a ping thread. Ping/pong are handled internally:
// the receive side answers pings, measures round-trip time and the peer clock offset (stats()),
// and declares the connection dead after kDeadAfter without any incoming data.
#pragma once

#include <winsock2.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <stop_token>
#include <thread>

#include <nlohmann/json.hpp>

#include "kvm/protocol.h"
#include "kvm/queue.h"
#include "kvm/tls.h"

namespace kvm {

// Monotonic clock in microseconds. Same clock for ping timestamps and FrameHeader::t_capture_us.
std::int64_t steady_now_us();

}  // namespace kvm

namespace kvm::net {

inline constexpr auto kPingInterval = std::chrono::seconds(1);
// Phone hotspots stall for several seconds now and then; a session that survives a stall keeps the
// virtual display, its windows and the clipboard in place. A closed connection is noticed at once.
inline constexpr auto kDeadAfter = std::chrono::seconds(10);

// Call once per process before any socket use (Server/connect do it for you). Thread-safe.
void ensure_winsock();

struct LinkStats {
    std::int64_t rtt_us = -1;          // last measured round trip, -1 = not measured yet
    std::int64_t clock_offset_us = 0;  // peer_clock - local_clock, estimated from ping/pong
};

// A TLS connection right after the handshake: the peer's key is known (stream->peer_id()), and
// nothing has been read from it yet. Decide whether to trust it before making a Connection.
struct Handshaken {
    std::unique_ptr<tls::Stream> stream;
    std::string peer;  // "ip:port"
};

inline constexpr auto kHandshakeTimeout = std::chrono::seconds(10);

class Connection {
public:
    // Starts the receive and ping threads: only for a peer that has been checked.
    explicit Connection(Handshaken connection);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Sends one control message. Returns false if the connection is (or just became) closed.
    template <class M>
    bool send(const M& msg) {
        return send_json(proto::encode(msg));
    }
    bool send_json(const nlohmann::json& j);

    // Received messages except ping/pong. Closed when the connection drops.
    BlockingQueue<proto::Message>& incoming() { return incoming_; }

    bool is_open() const { return open_; }
    const std::string& peer() const { return peer_; }
    LinkStats stats() const;

    // Converts a timestamp from the peer's steady_now_us() clock into ours.
    std::int64_t peer_to_local_us(std::int64_t peer_us) const { return peer_us - clock_offset_us_; }

    // Closes the socket; receive/ping threads stop and incoming() gets closed. Idempotent.
    void close();

private:
    void receive_loop();
    void ping_loop(std::stop_token stop);
    void handle_pong(const proto::Pong& pong);  // receive thread only

    struct OffsetSample {
        std::int64_t rtt_us = 0;
        std::int64_t offset_us = 0;
    };
    static constexpr std::size_t kOffsetSamples = 8;

    std::unique_ptr<tls::Stream> tls_;
    std::string peer_;
    std::atomic<bool> open_{true};
    std::mutex send_mutex_;
    BlockingQueue<proto::Message> incoming_;
    std::atomic<std::int64_t> rtt_us_{-1};
    std::atomic<std::int64_t> clock_offset_us_{0};
    std::array<OffsetSample, kOffsetSamples> offset_samples_{};  // receive thread only
    std::size_t offset_sample_count_ = 0;
    std::stop_source stop_;  // owned here (not by ping_thread_) so close() is safe from any thread
    std::jthread receive_thread_;
    std::jthread ping_thread_;
};

class Server {
public:
    // Listens on 0.0.0.0:port. Throws std::runtime_error if the port cannot be bound.
    explicit Server(std::uint16_t port);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Blocks until a client connects and completes the TLS handshake. A client whose handshake fails
    // (not TLS, no certificate, too slow) is logged and skipped. nullopt after stop().
    std::optional<Handshaken> accept(const tls::Credentials& me,
                                     std::chrono::milliseconds handshake_timeout = kHandshakeTimeout);

    // Unblocks accept() from any thread (e.g. a Ctrl+C handler). Idempotent.
    void stop();

private:
    std::atomic<SOCKET> listen_sock_{INVALID_SOCKET};
};

// ---- side channels: video (50501) and files (50502), docs/SECURITY.md 6.4 ---------------------
// They run TLS with the same keys as the control channel. The client's first frame is a
// channel_hello carrying the token the host sent in its hello. The host accepts a side channel only
// if its key is the live control session's peer and the token matches.

// The live control session that side channels may join. Thread-safe.
class SessionKey {
public:
    // A new session with `peer`: draws a fresh 32-byte token and returns it as hex.
    std::string open(const tls::DeviceId& peer);
    // No session: every side channel is refused. Waits for an install() running in admit().
    void close();
    // True if `peer` with `token` belongs to the live session. Then `install` (if given) runs before
    // this returns, under the lock close() takes: a channel installed there is either in place
    // before close() returns, for the caller to drop with the session, or never installed.
    bool admit(const tls::DeviceId& peer, std::string_view token, const std::function<void()>& install = {}) const;

private:
    mutable std::mutex mutex_;
    std::optional<tls::DeviceId> peer_;
    std::string token_;
};

// Host: reads the side channel's channel_hello within `timeout` and checks it against `session`
// (SessionKey::admit, which runs `install`). Logs a refusal (`what` = "video" / "files").
// True = the channel belongs to the live session. `install` may move the stream away: nothing
// here touches `stream` or `peer` after it.
bool admit_side_channel(tls::Stream& stream, const SessionKey& session, std::string_view what, std::string_view peer,
                        const std::function<void()>& install = {},
                        std::chrono::milliseconds timeout = std::chrono::seconds(5));

// Client: TLS to host:port; the host must have `host_id` (the control session's key); then sends
// channel_hello with `token`. nullopt on failure (logged).
std::optional<Handshaken> connect_side_channel(const std::string& host, std::uint16_t port, const tls::Credentials& me,
                                               const tls::DeviceId& host_id, std::string_view token,
                                               std::chrono::milliseconds timeout = std::chrono::seconds(2));

// Connects to host:port within `timeout` and runs the TLS handshake. nullopt on failure (logged);
// the caller decides when to retry.
std::optional<Handshaken> connect(const std::string& host, std::uint16_t port, const tls::Credentials& me,
                                  std::chrono::milliseconds timeout = std::chrono::seconds(2));

}  // namespace kvm::net
