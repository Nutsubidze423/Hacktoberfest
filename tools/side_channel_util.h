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
