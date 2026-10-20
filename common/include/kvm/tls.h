// SPDX-License-Identifier: Apache-2.0

// TLS 1.3 for Sarke's TCP channels (docs/SECURITY.md, section 6.2), on OpenSSL 3. Nothing here is
// hand-rolled: keys, certificates, the handshake, record protection and the keying-material
// exporter are OpenSSL's.
//
//   auto me = kvm::tls::Credentials::generate("DESKTOP-1");          // or load() a stored one
//   std::string why;
//   auto tls = kvm::tls::Stream::handshake(sock, kvm::tls::Role::Server, me, 10s, why);
//   if (!tls) log::warn("{}", why);                                   // the socket is closed
//   tls->peer_id();                                                   // pin this (SECURITY.md 6.4)
//   tls->send_all(data, size);  tls->recv_exact(buf, size);           // like net::detail's
//
// Both sides present a certificate (mutual TLS). Certificates are self-signed and accepted as
// such: trust comes from pinning the peer's device ID after the handshake, never from a CA.
// TLS 1.3 only, no session resumption, no early data: every connection is a full handshake.
//
// Threads: one thread may read while others write. The socket is non-blocking, every OpenSSL call
// runs under one lock, and waiting happens in select() outside it, so a writer never waits for a
// blocked reader. A send_all() is never interleaved with another one. abort() unblocks both.
#pragma once

#include <winsock2.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kvm::tls {

// SHA-256 of the certificate's DER SubjectPublicKeyInfo: who a peer is (SECURITY.md 6.1).
using DeviceId = std::array<std::uint8_t, 32>;

// "1A2B-3C4D-5E6F-7081": the first 8 bytes, for people.
std::string short_id(const DeviceId& id);

// What handshake() says when the peer starts with Sarke 1.1's plaintext hello (protocol v2).
inline constexpr std::string_view kOlderSarke =
    "an older Sarke (protocol v2) tried to connect - update both PCs to the same Sarke version";
// All 32 bytes as lowercase hex, for files and logs.
std::string to_hex(const DeviceId& id);
std::optional<DeviceId> from_hex(std::string_view hex);

// A device's identity: an ECDSA P-256 private key and its self-signed certificate.
class Credentials {
public:
    // A new key pair and certificate. `name` goes into the certificate's CN (shown, never trusted).
    static Credentials generate(std::string_view name);
    // From what key_der() / cert_der() returned. nullopt if either doesn't parse or they don't match.
    static std::optional<Credentials> load(std::span<const std::uint8_t> key_der,
                                           std::span<const std::uint8_t> cert_der);

    Credentials(Credentials&&) noexcept;
    Credentials& operator=(Credentials&&) noexcept;
    ~Credentials();

    std::vector<std::uint8_t> key_der() const;  // PKCS#8 DER: the secret, store it protected
    std::vector<std::uint8_t> cert_der() const;
    const DeviceId& id() const;

    struct Impl;
    const Impl& impl() const { return *impl_; }

private:
    explicit Credentials(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

enum class Role { Client, Server };

// A TLS 1.3 connection over a connected TCP socket, which it owns from handshake() on.
class Stream {
public:
    // Runs the handshake within `timeout`. On failure returns nullptr, closes the socket and says
    // why in `error` (e.g. "the peer doesn't speak TLS", "no certificate from the peer"). A server
    // recognises an older Sarke's plaintext hello and says so (kOlderSarke, docs/SECURITY.md 6.7).
    static std::unique_ptr<Stream> handshake(SOCKET socket, Role role, const Credentials& me,
                                             std::chrono::milliseconds timeout, std::string& error);
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    // Blocking send of the whole buffer. False on error or after abort().
    bool send_all(const char* data, std::size_t size);
    // Blocking receive of exactly `size` bytes. 0 on success, otherwise a WSA error code
    // (WSAECONNRESET also for an orderly close by the peer, WSAECONNABORTED after abort(),
    // WSAETIMEDOUT when nothing arrived for the receive timeout).
    int recv_exact(char* data, std::size_t size);
    // Like SO_RCVTIMEO: recv_exact() gives up after this long without any data. 0 = never (default).
    void set_receive_timeout(std::chrono::milliseconds timeout);
    // Makes blocked and later send_all() / recv_exact() calls return at once. Any thread.
    void abort();

    const DeviceId& peer_id() const { return peer_id_; }
    std::string_view peer_name() const { return peer_name_; }  // the peer certificate's CN (shown only)

    // RFC 8446 section 7.5 keying-material exporter: the same bytes on both ends of this session,
    // different bytes on the two halves of a relay. Empty on failure.
    std::vector<std::uint8_t> export_keying_material(std::string_view label, std::span<const std::uint8_t> context,
                                                     std::size_t length) const;

private:
    struct Impl;
    explicit Stream(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    DeviceId peer_id_{};
    std::string peer_name_;
};

// `bytes` random bytes from OpenSSL's CSPRNG, as lowercase hex (e.g. a session token).
std::string random_hex(std::size_t bytes);
// Compares two secrets in constant time (for tokens; the lengths themselves aren't secret).
bool equal_secret(std::string_view a, std::string_view b);

// The 6-digit pairing code from 4 exporter bytes (SECURITY.md 6.3): big-endian u32 mod 1 000 000,
// as "042 917".
std::string pairing_code(std::span<const std::uint8_t, 4> material);

}  // namespace kvm::tls
