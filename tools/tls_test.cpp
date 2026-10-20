// SPDX-License-Identifier: Apache-2.0

// tls_test: kvm/tls.h on 127.0.0.1 (roadmap P1.2, docs/SECURITY.md). Checks:
//   - credentials: generate, store as DER, load back with the same device ID; mismatched or broken
//     DER is refused; device-ID text forms; the pairing-code mapping against SECURITY.md's vectors;
//   - a mutual TLS 1.3 handshake: each side sees the other's device ID and the same exporter bytes;
//   - data both ways, 1 MB at once, and full duplex with two writers whose messages never interleave;
//   - abort() unblocks a waiting read; a closed peer ends the read;
//   - refused: a plaintext peer (like Sarke protocol v2), a silent peer (timeout), a TLS 1.2 client,
//     a client without a certificate;
//   - a relaying man-in-the-middle: each side sees the relay's key (pinning catches it) and the two
//     sessions export different bytes (the pairing codes differ, SECURITY.md 6.3).
//
//   tls_test.exe   (exit code 0 = all passed)

#include <winsock2.h>
#include <windows.h>

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "kvm/log.h"
#include "kvm/net.h"
#include "kvm/tls.h"

namespace tls = kvm::tls;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

struct Checks {
    int failed = 0;
    void operator()(bool ok, std::string_view what) {
        kvm::log::info("{} {}", ok ? "[ok]  " : "[FAIL]", what);
        if (!ok) ++failed;
    }
};

// A connected pair of blocking TCP sockets on 127.0.0.1: {client end, server end}.
std::pair<SOCKET, SOCKET> socket_pair() {
    kvm::net::ensure_winsock();
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof(addr);
    bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(listener, 1);
    getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    const SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    const SOCKET server = accept(listener, nullptr, nullptr);
    closesocket(listener);
    return {client, server};
}

struct Pair {
    std::unique_ptr<tls::Stream> client, server;
    std::string client_error, server_error;
};

// Handshakes both ends of a fresh socket pair at the same time.
Pair handshake_pair(const tls::Credentials& client_creds, const tls::Credentials& server_creds,
                    std::chrono::milliseconds timeout = 5s) {
    const auto [c, s] = socket_pair();
    Pair p;
    std::thread server(
        [&] { p.server = tls::Stream::handshake(s, tls::Role::Server, server_creds, timeout, p.server_error); });
    p.client = tls::Stream::handshake(c, tls::Role::Client, client_creds, timeout, p.client_error);
    server.join();
    return p;
}

std::vector<std::uint8_t> pairing_context(const tls::DeviceId& a, const tls::DeviceId& b) {
    const auto& [low, high] = std::minmax(a, b);
    std::vector<std::uint8_t> context(low.begin(), low.end());
    context.insert(context.end(), high.begin(), high.end());
    return context;
}

std::vector<std::uint8_t> pairing_material(const tls::Stream& s, const tls::DeviceId& a, const tls::DeviceId& b) {
    return s.export_keying_material("EXPORTER-sarke-pairing-v1", pairing_context(a, b), 4);
}

// [u32 size][size bytes], the framing the control channel uses.
bool send_message(tls::Stream& s, const std::string& payload) {
    std::string frame(4 + payload.size(), '\0');
    const auto n = static_cast<std::uint32_t>(payload.size());
    frame[0] = static_cast<char>(n >> 24);
    frame[1] = static_cast<char>(n >> 16);
    frame[2] = static_cast<char>(n >> 8);
    frame[3] = static_cast<char>(n);
    std::memcpy(frame.data() + 4, payload.data(), payload.size());
    return s.send_all(frame.data(), frame.size());
}

std::optional<std::string> recv_message(tls::Stream& s) {
    unsigned char head[4];
    if (s.recv_exact(reinterpret_cast<char*>(head), 4) != 0) return std::nullopt;
    const std::uint32_t n = static_cast<std::uint32_t>(head[0]) << 24 | static_cast<std::uint32_t>(head[1]) << 16 |
                            static_cast<std::uint32_t>(head[2]) << 8 | head[3];
    std::string payload(n, '\0');
    if (n && s.recv_exact(payload.data(), n) != 0) return std::nullopt;
    return payload;
}

// A message whose content proves it arrived whole: "<writer>:<seq>:" padded with a byte pattern.
std::string make_message(int writer, int seq) {
    std::string m = std::format("{}:{}:", writer, seq);
    const std::size_t size = 50 + static_cast<std::size_t>(seq * 37 % 3000);
    while (m.size() < size) m.push_back(static_cast<char>('a' + (m.size() + writer) % 26));
    return m;
}

bool message_intact(const std::string& m, int& writer, int& seq) {
    const char* const end = m.data() + m.size();
    auto [p1, e1] = std::from_chars(m.data(), end, writer);
    if (e1 != std::errc() || p1 == end || *p1 != ':') return false;
    auto [p2, e2] = std::from_chars(p1 + 1, end, seq);
    if (e2 != std::errc() || p2 == end || *p2 != ':') return false;
    return m == make_message(writer, seq);
}

// A plain OpenSSL client for the refusal checks: TLS 1.2 at most, or without a certificate.
struct RawClient {
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    ~RawClient() {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
    }
};

bool raw_handshake(SOCKET sock, int max_version, const tls::Credentials* cert_from) {
    RawClient c;
    c.ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(c.ctx, SSL_VERIFY_NONE, nullptr);
    if (max_version) SSL_CTX_set_max_proto_version(c.ctx, max_version);
    if (cert_from) {
        const auto key = cert_from->key_der();
        const auto cert = cert_from->cert_der();
        const unsigned char* k = key.data();
        const unsigned char* x = cert.data();
        EVP_PKEY* pkey = d2i_AutoPrivateKey(nullptr, &k, static_cast<long>(key.size()));
        X509* x509 = d2i_X509(nullptr, &x, static_cast<long>(cert.size()));
        SSL_CTX_use_certificate(c.ctx, x509);
        SSL_CTX_use_PrivateKey(c.ctx, pkey);
        X509_free(x509);
        EVP_PKEY_free(pkey);
    }
    c.ssl = SSL_new(c.ctx);
    SSL_set_fd(c.ssl, static_cast<int>(sock));
    const bool ok = SSL_connect(c.ssl) == 1;
    // In TLS 1.3 the client may finish before the server rejects its (missing) certificate: read once
    // to see the server's verdict.
    char byte = 0;
    const bool still_open = ok && SSL_read(c.ssl, &byte, 1) > 0;
    closesocket(sock);
    return ok && still_open;
}

}  // namespace

int main() {
    Checks check;

    kvm::log::info("credentials");
    const auto alice = tls::Credentials::generate("alice-pc");
    const auto bob = tls::Credentials::generate("bob-pc");
    const auto mallory = tls::Credentials::generate("mallory-pc");
    check(alice.id() != tls::DeviceId{} && alice.id() != bob.id(), "new keys have distinct, non-zero device IDs");
    const auto key = alice.key_der();
    const auto cert = alice.cert_der();
    const auto loaded = tls::Credentials::load(key, cert);
    check(loaded && loaded->id() == alice.id(), "stored as DER and loaded back: the same device ID");
    check(!tls::Credentials::load(bob.key_der(), cert), "a key that doesn't match the certificate is refused");
    const std::vector<std::uint8_t> garbage(100, 0x42);
    check(!tls::Credentials::load(garbage, garbage), "garbage DER is refused");
    const auto hex = tls::to_hex(alice.id());
    check(hex.size() == 64 && tls::from_hex(hex) == alice.id() && !tls::from_hex("zz"), "device ID as hex, both ways");
    const auto short_text = tls::short_id(alice.id());
    std::string expected_short;
    for (std::size_t i = 0; i < 16; ++i) {
        if (i && i % 4 == 0) expected_short += '-';
        expected_short += static_cast<char>(std::toupper(static_cast<unsigned char>(hex[i])));
    }
    check(short_text == expected_short, std::format("short form {} (the first 8 bytes)", short_text));

    // SECURITY.md section 8: the code mapping.
    const std::array<std::pair<std::array<std::uint8_t, 4>, std::string_view>, 5> vectors{{
        {{0x00, 0x00, 0x00, 0x00}, "000 000"},
        {{0x00, 0x0F, 0x42, 0x3F}, "999 999"},
        {{0x00, 0x0F, 0x42, 0x40}, "000 000"},
        {{0x12, 0x34, 0x56, 0x78}, "419 896"},
        {{0xFF, 0xFF, 0xFF, 0xFF}, "967 295"},
    }};
    bool vectors_ok = true;
    for (const auto& [bytes, code] : vectors) vectors_ok = vectors_ok && tls::pairing_code(bytes) == code;
    check(vectors_ok, "pairing codes match SECURITY.md's test vectors");

    kvm::log::info("mutual TLS 1.3 handshake");
    {
        auto p = handshake_pair(alice, bob);
        check(p.client && p.server,
              std::format("both sides complete it{}{}", p.client_error.empty() ? "" : " / client: " + p.client_error,
                          p.server_error.empty() ? "" : " / server: " + p.server_error));
        if (p.client && p.server) {
            check(p.client->peer_id() == bob.id() && p.server->peer_id() == alice.id(),
                  "each side sees the other's device ID");
            check(p.client->peer_name() == "bob-pc" && p.server->peer_name() == "alice-pc", "and its name");
            const auto a = pairing_material(*p.client, alice.id(), bob.id());
            const auto b = pairing_material(*p.server, bob.id(), alice.id());
            check(a.size() == 4 && a == b,
                  std::format("the same exporter bytes on both ends: code {}",
                              a.size() == 4 ? tls::pairing_code(std::span<const std::uint8_t, 4>(a.data(), 4)) : "?"));

            kvm::log::info("data");
            std::string big(1 << 20, '\0');
            for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31 + 7);
            auto sent = std::async(std::launch::async, [&] { return p.client->send_all(big.data(), big.size()); });
            std::string got(big.size(), '\0');
            const int r = p.server->recv_exact(got.data(), got.size());
            check(sent.get() && r == 0 && got == big, "1 MB client -> server arrives intact");
            auto back = std::async(std::launch::async, [&] { return p.server->send_all(big.data(), big.size()); });
            std::fill(got.begin(), got.end(), '\0');
            check(p.client->recv_exact(got.data(), got.size()) == 0 && back.get() && got == big,
                  "1 MB server -> client arrives intact");

            // Full duplex: the server streams to the client while two client threads write to the
            // server; each side reads on its own thread.
            constexpr int kPerWriter = 300, kFromServer = 1000;
            std::atomic<int> server_got{0}, server_bad{0}, client_got{0}, client_bad{0};
            std::thread server_reader([&] {
                std::array<int, 2> next{0, 0};
                for (int i = 0; i < 2 * kPerWriter; ++i) {
                    const auto m = recv_message(*p.server);
                    int w = -1, seq = -1;
                    if (!m || !message_intact(*m, w, seq) || w < 0 || w > 1 || seq != next[w]++) {
                        ++server_bad;
                        return;
                    }
                    ++server_got;
                }
            });
            std::thread client_reader([&] {
                for (int i = 0; i < kFromServer; ++i) {
                    const auto m = recv_message(*p.client);
                    int w = -1, seq = -1;
                    if (!m || !message_intact(*m, w, seq) || w != 9 || seq != i) {
                        ++client_bad;
                        return;
                    }
                    ++client_got;
                }
            });
            std::thread server_writer([&] {
                for (int i = 0; i < kFromServer; ++i) send_message(*p.server, make_message(9, i));
            });
            std::thread writer0([&] {
                for (int i = 0; i < kPerWriter; ++i) send_message(*p.client, make_message(0, i));
            });
            std::thread writer1([&] {
                for (int i = 0; i < kPerWriter; ++i) send_message(*p.client, make_message(1, i));
            });
            writer0.join();
            writer1.join();
            server_writer.join();
            server_reader.join();
            client_reader.join();
            check(server_got == 2 * kPerWriter && server_bad == 0,
                  std::format("two writers, {} messages, none interleaved or lost", server_got.load()));
            check(client_got == kFromServer && client_bad == 0,
                  std::format("full duplex: {} messages the other way meanwhile", client_got.load()));

            kvm::log::info("abort and close");
            auto waiting = std::async(std::launch::async, [&] {
                char byte = 0;
                return p.client->recv_exact(&byte, 1);
            });
            std::this_thread::sleep_for(300ms);
            const auto t0 = Clock::now();
            p.client->abort();
            const bool unblocked = waiting.wait_for(3s) == std::future_status::ready;
            check(unblocked && waiting.get() == WSAECONNABORTED,
                  std::format("abort() unblocks a waiting read ({} ms)",
                              std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()));
            check(!p.client->send_all("x", 1), "and later sends fail");
        }
    }
    {
        auto p = handshake_pair(alice, bob);
        if (p.client && p.server) {
            auto reading = std::async(std::launch::async, [&] {
                char byte = 0;
                return p.server->recv_exact(&byte, 1);
            });
            p.client.reset();  // close_notify, then the socket closes
            check(reading.wait_for(3s) == std::future_status::ready && reading.get() != 0,
                  "the peer closing ends a waiting read");
        } else {
            check(false, "second handshake");
        }
    }

    kvm::log::info("refused");
    {
        // Sarke protocol v2 speaks plain [u32 len][JSON] where TLS expects a ClientHello.
        const auto [c, s] = socket_pair();
        const char v2_hello[] = "\x00\x00\x00\x12{\"type\":\"hello\"}  ";
        send(c, v2_hello, sizeof(v2_hello) - 1, 0);
        std::string why;
        const auto t0 = Clock::now();
        const auto stream = tls::Stream::handshake(s, tls::Role::Server, bob, 5s, why);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        closesocket(c);
        check(!stream && !why.empty() && ms < 2000, std::format("a plaintext peer, in {} ms: {}", ms, why));
    }
    {
        const auto [c, s] = socket_pair();
        std::string why;
        const auto stream = tls::Stream::handshake(s, tls::Role::Server, bob, 1s, why);
        closesocket(c);
        check(!stream && why.find("no handshake within") != std::string::npos, std::format("a silent peer: {}", why));
    }
    {
        const auto [c, s] = socket_pair();
        std::string why;
        std::unique_ptr<tls::Stream> stream;
        std::thread server([&] { stream = tls::Stream::handshake(s, tls::Role::Server, bob, 5s, why); });
        const bool client_ok = raw_handshake(c, TLS1_2_VERSION, &alice);
        server.join();
        check(!stream && !client_ok, std::format("a TLS 1.2 client: {}", why));
    }
    {
        const auto [c, s] = socket_pair();
        std::string why;
        std::unique_ptr<tls::Stream> stream;
        std::thread server([&] { stream = tls::Stream::handshake(s, tls::Role::Server, bob, 5s, why); });
        const bool client_ok = raw_handshake(c, 0, nullptr);
        server.join();
        check(!stream && !client_ok, std::format("a client without a certificate: {}", why));
    }

    kvm::log::info("a relaying man-in-the-middle");
    {
        // alice -> mallory (as server) ... mallory (as client) -> bob
        auto front = handshake_pair(alice, mallory);
        auto back = handshake_pair(mallory, bob);
        if (front.client && front.server && back.client && back.server) {
            check(front.client->peer_id() == mallory.id() && front.client->peer_id() != bob.id(),
                  "alice sees mallory's key, not bob's: pinning refuses it");
            check(back.server->peer_id() == mallory.id() && back.server->peer_id() != alice.id(),
                  "bob sees mallory's key, not alice's");
            // Even if both users ignored the keys: the codes they would compare differ.
            const auto alice_side = pairing_material(*front.client, alice.id(), mallory.id());
            const auto bob_side = pairing_material(*back.server, bob.id(), mallory.id());
            check(alice_side.size() == 4 && bob_side.size() == 4 && alice_side != bob_side,
                  std::format("the two halves export different bytes: {} vs {}",
                              tls::pairing_code(std::span<const std::uint8_t, 4>(alice_side.data(), 4)),
                              tls::pairing_code(std::span<const std::uint8_t, 4>(bob_side.data(), 4))));
        } else {
            check(false, "relay handshakes");
        }
    }

    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
