// SPDX-License-Identifier: Apache-2.0

// Tests: two PCs' keys and a live control session, so a test opens the video and file side
// channels the way kvm_host and kvm_client do (kvm/net.h, docs/SECURITY.md 6.4).
//
//   kvm::test::Session s;                                    // the client may join with s.token
//   std::thread host([&] { pending = kvm::test::accept_side_channel(server, s, "video"); });
//   auto client = kvm::net::connect_video("127.0.0.1", port, s.client, s.host.id(), s.token);
#pragma once

#include <winsock2.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "kvm/net.h"
#include "kvm/tls.h"

namespace kvm::test {

struct Session {
    tls::Credentials host = tls::Credentials::generate("test-host");
    tls::Credentials client = tls::Credentials::generate("test-client");
    net::SessionKey key;
    std::string token = key.open(client.id());
};

// Host side: one accept on `server` (a VideoServer or FileServer), admitted like kvm_host does.
// nullopt if the channel was refused (it is closed) or the server stopped.
template <class Server>
std::optional<net::Handshaken> accept_side_channel(Server& server, const Session& s, std::string_view what) {
    auto pending = server.accept(s.host);
    if (!pending || !net::admit_side_channel(*pending->stream, s.key, what, pending->peer)) return std::nullopt;
    return pending;
}

// Listens on 127.0.0.1 only (a free port), so Windows Firewall doesn't ask anything. Accept from it
// and hand the socket to admit_socket().
inline SOCKET listen_loopback(std::uint16_t& port) {
    net::ensure_winsock();
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof(addr);
    if (s == INVALID_SOCKET || ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(s, 1) != 0 || ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        if (s != INVALID_SOCKET) closesocket(s);
        return INVALID_SOCKET;
    }
    port = ntohs(addr.sin_port);
    return s;
}

// Host side over a socket the test accepted itself (a 127.0.0.1-only listener, so Windows Firewall
// asks nothing): TLS, then the channel_hello check. nullopt if refused.
inline std::optional<net::Handshaken> admit_socket(SOCKET sock, const Session& s, std::string_view what,
                                                   std::string peer) {
    if (sock == INVALID_SOCKET) return std::nullopt;
    std::string why;
    auto stream = tls::Stream::handshake(sock, tls::Role::Server, s.host, std::chrono::seconds(5), why);
    if (!stream || !net::admit_side_channel(*stream, s.key, what, peer)) return std::nullopt;
    return net::Handshaken{std::move(stream), std::move(peer)};
}

}  // namespace kvm::test
