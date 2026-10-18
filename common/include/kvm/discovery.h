// Finding the host on the local network without typing its IP (UDP port 50503).
//
//   A:  kvm::discovery::Responder responder(hostname, cfg.control_port);   // answers while alive
//   B:  auto hosts = kvm::discovery::find();                               // ~1.5 s, every host found
//
// B broadcasts {"type":"kvm_discover"} to 255.255.255.255 and to the broadcast address of every
// IPv4 network it is on; each host answers B directly with {"type":"kvm_here", name, ports,
// protocol_version, busy}. Only A needs an inbound firewall rule (UDP 50503): Windows lets the unicast
// answer to B's own broadcast through.
#pragma once

#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace kvm::discovery {

inline constexpr std::uint16_t kDiscoveryPort = 50503;

struct Host {
    std::string name;  // computer name
    std::string ip;    // where the answer came from
    std::uint16_t control_port = 0;
    int protocol_version = 0;
    bool busy = false;  // a client is connected to it right now
};

// Answers discovery queries until destroyed (own thread). Throws std::runtime_error if the port
// cannot be bound (e.g. another kvm_host is running).
class Responder {
public:
    Responder(std::string name, std::uint16_t control_port, std::uint16_t port = kDiscoveryPort);
    ~Responder();

    Responder(const Responder&) = delete;
    Responder& operator=(const Responder&) = delete;

    // A client is connected: answers say "busy", so other PCs list this one as taken.
    void set_busy(bool busy) { busy_ = busy; }

private:
    void run();

    std::string name_;
    std::uint16_t control_port_;
    SOCKET sock_ = INVALID_SOCKET;
    std::atomic<bool> running_{true};
    std::atomic<bool> busy_{false};
    std::thread thread_;
};

// Broadcasts queries for `wait` and returns every host that answered (one entry per IP, sorted by
// name). Empty if nobody answered.
std::vector<Host> find(std::chrono::milliseconds wait = std::chrono::milliseconds(1500),
                       std::uint16_t port = kDiscoveryPort);

}  // namespace kvm::discovery
