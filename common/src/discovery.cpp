#include "kvm/discovery.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>

#include "kvm/log.h"
#include "kvm/net.h"
#include "kvm/protocol.h"
#include "socket_util.h"

namespace kvm::discovery {

using namespace net::detail;

namespace {

constexpr std::string_view kQuery = "kvm_discover";
constexpr std::string_view kAnswer = "kvm_here";
constexpr std::size_t kMaxDatagram = 1024;

std::string ip_of(const sockaddr_in& addr) {
    std::array<char, INET_ADDRSTRLEN> buf{};
    inet_ntop(AF_INET, &addr.sin_addr, buf.data(), buf.size());
    return buf.data();
}

// 255.255.255.255 plus the directed broadcast address (e.g. 172.20.10.15) of every IPv4 network
// this PC is on: some Wi-Fi routers and hotspots only forward one of the two.
std::vector<in_addr> broadcast_addresses() {
    std::vector<in_addr> out;
    in_addr all{};
    all.s_addr = INADDR_BROADCAST;
    out.push_back(all);

    ULONG size = 16 * 1024;
    std::vector<std::byte> buffer(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc != NO_ERROR) {
        return out;
    }
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            const ULONG prefix = u->OnLinkPrefixLength;
            if (sin->sin_family != AF_INET || prefix == 0 || prefix >= 31) continue;
            const ULONG host_bits = (prefix == 32) ? 0 : (0xFFFFFFFFu >> prefix);
            in_addr bcast{};
            bcast.s_addr = htonl(ntohl(sin->sin_addr.s_addr) | host_bits);
            if (std::none_of(out.begin(), out.end(), [&](const in_addr& x) { return x.s_addr == bcast.s_addr; })) {
                out.push_back(bcast);
            }
        }
    }
    return out;
}

void set_receive_timeout(SOCKET s, DWORD ms) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

}  // namespace

Responder::Responder(std::string name, std::uint16_t control_port, std::uint16_t port)
    : name_(std::move(name)), control_port_(control_port) {
    net::ensure_winsock();
    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) {
        throw std::runtime_error("discovery: socket() failed: " + last_error_text());
    }
    BOOL exclusive = TRUE;
    setsockopt(sock_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(sock_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        const std::string err = last_error_text();
        closesocket(sock_);
        throw std::runtime_error("discovery: cannot use UDP port " + std::to_string(port) + ": " + err);
    }
    set_receive_timeout(sock_, 500);  // lets run() notice the destructor
    thread_ = std::thread([this] { run(); });
}

Responder::~Responder() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
    closesocket(sock_);
}

void Responder::run() {
    std::array<char, kMaxDatagram> buf{};
    // One log line per asker and minute, not one per query: Sarke on every PC of the network asks
    // every few seconds to keep its device list current.
    std::map<std::string, std::chrono::steady_clock::time_point> last_log;
    while (running_) {
        sockaddr_in from{};
        int from_len = sizeof(from);
        const int got = recvfrom(sock_, buf.data(), static_cast<int>(buf.size()), 0,
                                 reinterpret_cast<sockaddr*>(&from), &from_len);
        if (got <= 0) {
            continue;  // timeout (checks running_) or an ICMP error from an earlier answer
        }
        const auto j = nlohmann::json::parse(std::string_view(buf.data(), static_cast<std::size_t>(got)), nullptr, false);
        if (j.is_discarded() || !j.is_object() || j.value("type", "") != kQuery) {
            continue;
        }
        const std::string answer = nlohmann::json{{"type", kAnswer},
                                                  {"name", name_},
                                                  {"control_port", control_port_},
                                                  {"protocol_version", proto::kProtocolVersion},
                                                  {"busy", busy_.load()}}
                                       .dump();
        sendto(sock_, answer.data(), static_cast<int>(answer.size()), 0, reinterpret_cast<const sockaddr*>(&from),
               from_len);
        const auto now = std::chrono::steady_clock::now();
        const std::string ip = ip_of(from);
        if (const auto it = last_log.find(ip); it == last_log.end() || now - it->second > std::chrono::minutes(1)) {
            log::info("discovery: answered {}", ip);
            if (last_log.size() > 64) last_log.clear();
            last_log[ip] = now;
        }
    }
}

std::vector<Host> find(std::chrono::milliseconds wait, std::uint16_t port) {
    net::ensure_winsock();
    std::vector<Host> hosts;
    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        log::warn("discovery: socket() failed: {}", last_error_text());
        return hosts;
    }
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    ::bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local));

    const std::string query = nlohmann::json{{"type", kQuery}, {"protocol_version", proto::kProtocolVersion}}.dump();
    const auto targets = broadcast_addresses();
    const auto send_queries = [&] {
        for (const auto& target : targets) {
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_addr = target;
            to.sin_port = htons(port);
            sendto(s, query.data(), static_cast<int>(query.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        }
    };

    // Ask three times across the wait: one lost datagram on Wi-Fi shouldn't hide a host.
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + wait;
    auto next_query = start;
    std::array<char, kMaxDatagram> buf{};
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        if (now >= next_query) {
            send_queries();
            next_query = now + wait / 3;
        }
        const auto until = std::min(deadline, next_query);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count();
        set_receive_timeout(s, static_cast<DWORD>(std::max<long long>(ms, 1)));

        sockaddr_in from{};
        int from_len = sizeof(from);
        const int got = recvfrom(s, buf.data(), static_cast<int>(buf.size()), 0, reinterpret_cast<sockaddr*>(&from),
                                 &from_len);
        if (got <= 0) continue;
        const auto j = nlohmann::json::parse(std::string_view(buf.data(), static_cast<std::size_t>(got)), nullptr, false);
        if (j.is_discarded() || !j.is_object() || j.value("type", "") != kAnswer) continue;

        Host h{.name = j.value("name", ""),
               .ip = ip_of(from),
               .control_port = j.value<std::uint16_t>("control_port", 0),
               .protocol_version = j.value("protocol_version", 0),
               .busy = j.value("busy", false)};
        if (h.control_port == 0) continue;
        if (std::none_of(hosts.begin(), hosts.end(), [&](const Host& x) { return x.ip == h.ip; })) {
            hosts.push_back(std::move(h));
        }
    }
    closesocket(s);
    std::sort(hosts.begin(), hosts.end(), [](const Host& a, const Host& b) { return a.name < b.name; });
    return hosts;
}

}  // namespace kvm::discovery
