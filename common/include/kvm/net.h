// TCP transport for the control channel (port 50500): length-prefixed JSON messages.
//
//   Host A:  kvm::net::Server server(port);  auto conn = server.accept();
//   Client B: auto conn = kvm::net::connect(ip, port, 2s);   // nullptr on failure, retry yourself
//
//   conn->send(proto::MouseMove{.dx = 3, .dy = -1});        // thread-safe, never throws
//   while (auto msg = conn->incoming().pop()) { ... }        // ends when the connection drops
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
#include <memory>
#include <mutex>
#include <string>
#include <stop_token>
#include <thread>

#include <nlohmann/json.hpp>

#include "kvm/protocol.h"
#include "kvm/queue.h"

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

class Connection {
public:
    // Takes ownership of a connected socket and starts the receive and ping threads.
    Connection(SOCKET sock, std::string peer);
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

    SOCKET sock_;
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

    // Blocks until a client connects. nullptr after stop().
    std::unique_ptr<Connection> accept();

    // Unblocks accept() from any thread (e.g. a Ctrl+C handler). Idempotent.
    void stop();

private:
    std::atomic<SOCKET> listen_sock_{INVALID_SOCKET};
};

// Connects to host:port within `timeout`. nullptr on failure (logged); caller decides when to retry.
std::unique_ptr<Connection> connect(const std::string& host, std::uint16_t port,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(2));

}  // namespace kvm::net
