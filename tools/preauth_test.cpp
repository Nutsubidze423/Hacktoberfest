// SPDX-License-Identifier: Apache-2.0

// preauth_test: what a PC that never completes a TLS handshake can do to kvm::net::Server (roadmap
// P1.12, docs/SECURITY.md S9). On 127.0.0.1; other loopback addresses (127.0.0.x) stand in for other
// PCs. Checks:
//   - a silent connection holds a handshake slot until its timeout, and one address gets 2 slots;
//   - the paired PC still gets in meanwhile;
//   - at most 4 handshakes run at once, whoever opens them;
//   - after 2 failed handshakes an address waits before it may try again (1 s, doubling to 30 s);
//   - stop() ends the handshakes still running at once.
//
//   preauth_test.exe   (exit code 0 = all passed)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include "kvm/log.h"
#include "kvm/net.h"
#include "kvm/tls.h"

using namespace std::chrono_literals;
namespace net = kvm::net;
namespace tls = kvm::tls;
using Clock = std::chrono::steady_clock;

namespace {

struct Checks {
    int failed = 0;
    void operator()(bool ok, std::string_view what) {
        kvm::log::info("{} {}", ok ? "[ok]  " : "[FAIL]", what);
        if (!ok) ++failed;
    }
};

std::uint16_t free_port() {
    net::ensure_winsock();
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    int len = sizeof(addr);
    std::uint16_t port = 0;
    if (s != INVALID_SOCKET && bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
        getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        port = ntohs(addr.sin_port);
    }
    if (s != INVALID_SOCKET) closesocket(s);
    return port;
}

// A plain TCP connection to 127.0.0.1:port from `source` (another loopback address = another PC).
SOCKET connect_from(const char* source, std::uint16_t port) {
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in from{};
    from.sin_family = AF_INET;
    inet_pton(AF_INET, source, &from.sin_addr);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&from), sizeof(from)) != 0 ||
        connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) != 0) {
        kvm::log::error("can't connect from {}: {}", source, WSAGetLastError());
        if (s != INVALID_SOCKET) closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

// The server closed `s` within `timeout` (nothing is ever sent to these connections).
bool closed_within(SOCKET s, std::chrono::milliseconds timeout) {
    if (s == INVALID_SOCKET) return false;
    const DWORD ms = static_cast<DWORD>(timeout.count());
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    char byte = 0;
    const int got = recv(s, &byte, 1, 0);
    return got == 0 || (got == SOCKET_ERROR && WSAGetLastError() != WSAETIMEDOUT);
}

// A Server on a free port with accept() running, as kvm_host has it; keeps what it accepts.
class TestServer {
public:
    TestServer(const tls::Credentials& me, std::chrono::milliseconds handshake_timeout)
        : port_(free_port()), server_(port_) {
        loop_ = std::jthread([this, &me, handshake_timeout] {
            while (auto accepted = server_.accept(me, handshake_timeout)) {
                std::lock_guard lock(mutex_);
                held_.push_back(std::move(*accepted));
            }
        });
    }
    ~TestServer() { server_.stop(); }  // the loop ends; ~Server waits for the handshakes
    std::uint16_t port() const { return port_; }
    std::size_t accepted() {
        std::lock_guard lock(mutex_);
        return held_.size();
    }
    net::Server& server() { return server_; }

private:
    std::uint16_t port_;
    net::Server server_;
    std::mutex mutex_;
    std::vector<net::Handshaken> held_;
    std::jthread loop_;  // last: joined before the members it uses go away
};

bool wait_until(const auto& done, std::chrono::milliseconds timeout) {
    const auto end = Clock::now() + timeout;
    while (Clock::now() < end) {
        if (done()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return done();
}

}  // namespace

int main() {
    Checks check;
    net::ensure_winsock();
    const auto host = tls::Credentials::generate("preauth-host");
    const auto client = tls::Credentials::generate("preauth-client");

    kvm::log::info("backoff after failed handshakes");
    {
        const net::HandshakeLimits limits;
        check(net::handshake_backoff(0, limits) == 0s && net::handshake_backoff(2, limits) == 0s,
              "the first 2 failures cost nothing");
        check(net::handshake_backoff(3, limits) == 1s && net::handshake_backoff(4, limits) == 2s &&
                  net::handshake_backoff(5, limits) == 4s && net::handshake_backoff(9, limits) == 30s &&
                  net::handshake_backoff(1000, limits) == 30s,
              "then 1 s, doubling, at most 30 s");
    }

    kvm::log::info("a silent connection");
    {
        TestServer server(host, 1500ms);
        const SOCKET silent = connect_from("127.0.0.2", server.port());
        const SOCKET second = connect_from("127.0.0.2", server.port());
        std::this_thread::sleep_for(300ms);  // their handshakes have started
        int refused = 0;
        for (int i = 0; i < 3; ++i) {
            const SOCKET more = connect_from("127.0.0.2", server.port());
            if (closed_within(more, 1s)) ++refused;
            if (more != INVALID_SOCKET) closesocket(more);
        }
        check(refused == 3,
              std::format("one address gets 2 slots: more from it are closed at once ({} of 3)", refused));
        check(!closed_within(silent, 300ms) && !closed_within(second, 10ms), "the first two hold their slots...");
        const auto t0 = Clock::now();
        check(closed_within(silent, 3s) && closed_within(second, 1s), "...until the handshake timeout");
        const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        kvm::log::info("       closed {} ms after that", held);
        closesocket(silent);
        closesocket(second);

        const SOCKET another = connect_from("127.0.0.3", server.port());
        std::this_thread::sleep_for(200ms);
        const auto pending = net::connect("127.0.0.1", server.port(), client, 3s);
        check(pending && wait_until([&] { return server.accepted() == 1; }, 2s),
              "while a silent one holds a slot, the paired PC still gets in");
        closesocket(another);
    }

    kvm::log::info("many addresses at once");
    {
        TestServer server(host, 3000ms);
        std::vector<SOCKET> silent;
        for (int i = 10; i < 14; ++i)
            silent.push_back(connect_from(std::format("127.0.0.{}", i).c_str(), server.port()));
        std::this_thread::sleep_for(300ms);
        const SOCKET fifth = connect_from("127.0.0.14", server.port());
        check(closed_within(fifth, 1s), "with 4 handshakes running, a 5th address is closed at once");
        int still_held = 0;
        for (const SOCKET s : silent) {
            if (!closed_within(s, 100ms)) ++still_held;
        }
        check(still_held == 4, std::format("and the 4 keep their slots ({} of 4)", still_held));
        for (const SOCKET s : silent) closesocket(s);
        closesocket(fifth);
    }

    kvm::log::info("failed handshakes");
    {
        TestServer server(host, 1500ms);
        int failed_fast = 0;
        for (int i = 0; i < 3; ++i) {
            const SOCKET s = connect_from("127.0.0.20", server.port());
            send(s, "GET / HTTP/1.1\r\n\r\n", 18, 0);
            if (closed_within(s, 1s)) ++failed_fast;
            closesocket(s);
            std::this_thread::sleep_for(50ms);
        }
        check(failed_fast == 3, "3 connections that don't speak TLS fail");
        const SOCKET waiting = connect_from("127.0.0.20", server.port());
        check(closed_within(waiting, 300ms), "then that address is closed at once: it has to wait 1 s");
        closesocket(waiting);
        const SOCKET other = connect_from("127.0.0.21", server.port());
        check(!closed_within(other, 300ms), "another address isn't");
        closesocket(other);
        std::this_thread::sleep_for(1200ms);
        const SOCKET later = connect_from("127.0.0.20", server.port());
        check(!closed_within(later, 300ms), "after the wait it may try again");
        closesocket(later);
    }

    kvm::log::info("stop()");
    {
        std::optional<TestServer> server;
        server.emplace(host, 10s);
        const SOCKET silent = connect_from("127.0.0.30", server->port());
        std::this_thread::sleep_for(300ms);
        const auto t0 = Clock::now();
        server.reset();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        check(ms < 1000, std::format("ends a running 10 s handshake at once ({} ms)", ms));
        check(closed_within(silent, 1s), "and closes its connection");
        closesocket(silent);
    }

    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
