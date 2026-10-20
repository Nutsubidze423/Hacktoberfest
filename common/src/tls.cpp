// SPDX-License-Identifier: Apache-2.0

#include "kvm/tls.h"

#include <winsock2.h>

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <stdexcept>

#include "socket_util.h"

namespace kvm::tls {

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

struct Free {
    void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
    void operator()(X509* p) const { X509_free(p); }
    void operator()(SSL_CTX* p) const { SSL_CTX_free(p); }
    void operator()(SSL* p) const { SSL_free(p); }
    void operator()(PKCS8_PRIV_KEY_INFO* p) const { PKCS8_PRIV_KEY_INFO_free(p); }
    void operator()(BIGNUM* p) const { BN_free(p); }
};
template <class T>
using Owned = std::unique_ptr<T, Free>;

// OpenSSL's error queue of this thread, as one line ("" if empty). Clears it.
std::string openssl_errors() {
    std::string text;
    while (const unsigned long code = ERR_get_error()) {
        char buf[256];
        ERR_error_string_n(code, buf, sizeof(buf));
        if (!text.empty()) text += "; ";
        text += buf;
    }
    return text;
}

DeviceId spki_id(X509* cert) {
    DeviceId id{};
    unsigned char* der = nullptr;
    const int len = i2d_PUBKEY(X509_get0_pubkey(cert), &der);
    unsigned int out = 0;
    if (len > 0) EVP_Digest(der, static_cast<std::size_t>(len), id.data(), &out, EVP_sha256(), nullptr);
    OPENSSL_free(der);
    return id;
}

std::string common_name(X509* cert) {
    char buf[256]{};
    const int len = X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, buf, sizeof(buf));
    return len > 0 ? std::string(buf, static_cast<std::size_t>(len)) : std::string();
}

// Self-signed certificates are expected: accept any certificate here, the caller pins the device
// ID after the handshake (SECURITY.md 6.2, 6.4).
int accept_any(int, X509_STORE_CTX*) {
    return 1;
}

enum class Wait { Ready, Timeout, Aborted, Error };

// Waits until `s` is readable / writable, in short slices so abort() is noticed promptly.
Wait wait_socket(SOCKET s, bool for_write, Clock::time_point deadline, const std::atomic<bool>& aborted) {
    for (;;) {
        if (aborted) return Wait::Aborted;
        const auto now = Clock::now();
        if (now >= deadline) return Wait::Timeout;
        const auto slice = std::min<Clock::duration>(deadline - now, 200ms);
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(s, &ready);
        fd_set failed;
        FD_ZERO(&failed);
        FD_SET(s, &failed);
        timeval tv{0, static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(slice).count())};
        const int n = select(0, for_write ? nullptr : &ready, for_write ? &ready : nullptr, &failed, &tv);
        // abort() cancels the socket's pending I/O with CancelIoEx, and select() waits through such
        // an I/O request: it then fails. That is an abort, not a socket error.
        if (n == SOCKET_ERROR) return aborted ? Wait::Aborted : Wait::Error;
        if (n > 0) return Wait::Ready;  // ready, or an error OpenSSL will now report
    }
}

constexpr auto kForever = Clock::time_point::max();

}  // namespace

// ---------------------------------------------------------------------------------------------
// Device IDs

std::string short_id(const DeviceId& id) {
    return std::format("{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}", id[0], id[1], id[2], id[3], id[4], id[5],
                       id[6], id[7]);
}

std::string to_hex(const DeviceId& id) {
    std::string text;
    text.reserve(id.size() * 2);
    for (const auto b : id) text += std::format("{:02x}", b);
    return text;
}

std::optional<DeviceId> from_hex(std::string_view hex) {
    if (hex.size() != 64) return std::nullopt;
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    DeviceId id{};
    for (std::size_t i = 0; i < id.size(); ++i) {
        const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        id[i] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    return id;
}

std::string random_hex(std::size_t bytes) {
    std::vector<unsigned char> raw(bytes);
    if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) throw std::runtime_error("tls: RAND_bytes failed");
    std::string text;
    text.reserve(bytes * 2);
    for (const auto b : raw) text += std::format("{:02x}", b);
    OPENSSL_cleanse(raw.data(), raw.size());
    return text;
}

bool equal_secret(std::string_view a, std::string_view b) {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string pairing_code(std::span<const std::uint8_t, 4> m) {
    const std::uint32_t value = static_cast<std::uint32_t>(m[0]) << 24 | static_cast<std::uint32_t>(m[1]) << 16 |
                                static_cast<std::uint32_t>(m[2]) << 8 | static_cast<std::uint32_t>(m[3]);
    const std::uint32_t code = value % 1'000'000;
    return std::format("{:03} {:03}", code / 1000, code % 1000);
}

// ---------------------------------------------------------------------------------------------
// Credentials

struct Credentials::Impl {
    Owned<EVP_PKEY> key;
    Owned<X509> cert;
    DeviceId id{};
};

Credentials::Credentials(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Credentials::Credentials(Credentials&&) noexcept = default;
Credentials& Credentials::operator=(Credentials&&) noexcept = default;
Credentials::~Credentials() = default;

Credentials Credentials::generate(std::string_view name) {
    auto impl = std::make_unique<Impl>();
    impl->key.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
    impl->cert.reset(X509_new());
    if (!impl->key || !impl->cert) throw std::runtime_error("tls: key generation failed: " + openssl_errors());
    X509* cert = impl->cert.get();

    unsigned char serial[16];
    RAND_bytes(serial, sizeof(serial));
    serial[0] &= 0x7F;  // positive
    Owned<BIGNUM> bn(BN_bin2bn(serial, sizeof(serial), nullptr));
    const bool ok = X509_set_version(cert, X509_VERSION_3) && bn &&
                    BN_to_ASN1_INTEGER(bn.get(), X509_get_serialNumber(cert)) &&
                    X509_gmtime_adj(X509_getm_notBefore(cert), -24 * 3600) &&
                    X509_time_adj_ex(X509_getm_notAfter(cert), 36500, 0, nullptr) &&  // ~100 years: pinned, not dated
                    X509_set_pubkey(cert, impl->key.get());
    // The CN is only shown to people: fall back to "Sarke" if the name doesn't fit (64 bytes).
    X509_NAME* subject = X509_get_subject_name(cert);
    const std::string cn = name.empty() || name.size() > 64 ? std::string("Sarke") : std::string(name);
    if (!X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>(cn.data()),
                                    static_cast<int>(cn.size()), -1, 0)) {
        X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("Sarke"), -1, -1,
                                   0);
    }
    if (!ok || !X509_set_issuer_name(cert, subject) || !X509_sign(cert, impl->key.get(), EVP_sha256())) {
        throw std::runtime_error("tls: certificate creation failed: " + openssl_errors());
    }
    impl->id = spki_id(cert);
    return Credentials(std::move(impl));
}

std::optional<Credentials> Credentials::load(std::span<const std::uint8_t> key_der,
                                             std::span<const std::uint8_t> cert_der) {
    auto impl = std::make_unique<Impl>();
    const unsigned char* k = key_der.data();
    impl->key.reset(d2i_AutoPrivateKey(nullptr, &k, static_cast<long>(key_der.size())));
    const unsigned char* c = cert_der.data();
    impl->cert.reset(d2i_X509(nullptr, &c, static_cast<long>(cert_der.size())));
    if (!impl->key || !impl->cert || !EVP_PKEY_is_a(impl->key.get(), "EC") ||
        X509_check_private_key(impl->cert.get(), impl->key.get()) != 1) {
        ERR_clear_error();
        return std::nullopt;
    }
    impl->id = spki_id(impl->cert.get());
    return Credentials(std::move(impl));
}

std::vector<std::uint8_t> Credentials::key_der() const {
    Owned<PKCS8_PRIV_KEY_INFO> p8(EVP_PKEY2PKCS8(impl_->key.get()));
    unsigned char* der = nullptr;
    const int len = p8 ? i2d_PKCS8_PRIV_KEY_INFO(p8.get(), &der) : -1;
    std::vector<std::uint8_t> out;
    if (len > 0) {
        out.assign(der, der + len);
        OPENSSL_clear_free(der, static_cast<std::size_t>(len));
    }
    return out;
}

std::vector<std::uint8_t> Credentials::cert_der() const {
    unsigned char* der = nullptr;
    const int len = i2d_X509(impl_->cert.get(), &der);
    std::vector<std::uint8_t> out;
    if (len > 0) out.assign(der, der + len);
    OPENSSL_free(der);
    return out;
}

const DeviceId& Credentials::id() const {
    return impl_->id;
}

// ---------------------------------------------------------------------------------------------
// Stream

struct Stream::Impl {
    SOCKET sock = INVALID_SOCKET;
    Owned<SSL_CTX> ctx;
    Owned<SSL> ssl;
    std::mutex ssl_mutex;  // every SSL_* call on `ssl` (OpenSSL objects aren't shared between threads)
    std::mutex write_mutex;  // one send_all() at a time, so messages never interleave
    std::atomic<bool> aborted{false};
    std::atomic<long long> receive_timeout_ms{0};  // 0 = wait forever

    ~Impl() {
        if (ssl && !aborted) {
            std::lock_guard lock(ssl_mutex);
            SSL_shutdown(ssl.get());  // close_notify, best effort (non-blocking)
        }
        ssl.reset();
        ctx.reset();
        if (sock != INVALID_SOCKET) closesocket(sock);
    }
};

Stream::Stream(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Stream::~Stream() = default;

std::unique_ptr<Stream> Stream::handshake(SOCKET socket, Role role, const Credentials& me,
                                          std::chrono::milliseconds timeout, std::string& error) {
    auto impl = std::make_unique<Impl>();
    impl->sock = socket;  // closed by ~Impl on every failure below
    const auto fail = [&](std::string why) -> std::unique_ptr<Stream> {
        error = std::move(why);
        return nullptr;
    };

    u_long non_blocking = 1;
    if (ioctlsocket(socket, FIONBIO, &non_blocking) != 0) return fail("tls: can't make the socket non-blocking");
    const auto deadline = Clock::now() + timeout;

    // A TLS client starts with a handshake record (0x16). Sarke 1.1 (protocol v2) starts with its
    // plaintext hello, [u32 big-endian length]{"type":"hello",...}: name that, rather than an OpenSSL
    // error, so the user learns which PC to update (docs/SECURITY.md 6.7). Nothing is consumed.
    if (role == Role::Server) {
        const Wait w = wait_socket(socket, false, deadline, impl->aborted);
        if (w == Wait::Timeout) return fail(std::format("tls: no handshake within {} ms", timeout.count()));
        if (w != Wait::Ready) return fail("tls: socket error during the handshake");
        unsigned char first[5] = {};
        const int got = recv(socket, reinterpret_cast<char*>(first), static_cast<int>(sizeof(first)), MSG_PEEK);
        if (got == 0) return fail("tls: the peer closed the connection during the handshake");
        if (got > 0 && first[0] != 0x16) {
            // A v2 frame's length is under 64 KB: two zero bytes, then (once it arrived) the JSON's '{'.
            const bool v2 = got >= 2 && first[0] == 0 && first[1] == 0 && (got < 5 || first[4] == '{');
            return fail(v2 ? std::string(kOlderSarke)
                           : std::format("tls: the peer doesn't speak TLS (it started with 0x{:02x})", first[0]));
        }
    }

    ERR_clear_error();
    const bool server = role == Role::Server;
    impl->ctx.reset(SSL_CTX_new(server ? TLS_server_method() : TLS_client_method()));
    SSL_CTX* ctx = impl->ctx.get();
    if (!ctx || !SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) ||
        !SSL_CTX_use_certificate(ctx, me.impl().cert.get()) || !SSL_CTX_use_PrivateKey(ctx, me.impl().key.get()) ||
        !SSL_CTX_check_private_key(ctx)) {
        return fail("tls: setup failed: " + openssl_errors());
    }
    // Both sides present a certificate; the server insists on the client's (SECURITY.md 6.2).
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | (server ? SSL_VERIFY_FAIL_IF_NO_PEER_CERT : 0), accept_any);
    // Every connection is a full handshake: no tickets, no session cache, no early data.
    SSL_CTX_set_num_tickets(ctx, 0);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    impl->ssl.reset(SSL_new(ctx));
    BIO* bio = impl->ssl ? BIO_new_socket(static_cast<int>(socket), BIO_NOCLOSE) : nullptr;
    if (!bio) return fail("tls: setup failed: " + openssl_errors());
    SSL_set_bio(impl->ssl.get(), bio, bio);
    if (server) {
        SSL_set_accept_state(impl->ssl.get());
    } else {
        SSL_set_connect_state(impl->ssl.get());
    }

    for (;;) {
        ERR_clear_error();
        const int r = SSL_do_handshake(impl->ssl.get());
        if (r == 1) break;
        const int e = SSL_get_error(impl->ssl.get(), r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            const Wait w = wait_socket(socket, e == SSL_ERROR_WANT_WRITE, deadline, impl->aborted);
            if (w == Wait::Timeout) {
                return fail(std::format("tls: no handshake within {} ms", timeout.count()));
            }
            if (w != Wait::Ready) return fail("tls: socket error during the handshake");
            continue;
        }
        if (e == SSL_ERROR_SYSCALL || e == SSL_ERROR_ZERO_RETURN) {
            const std::string details = openssl_errors();
            return fail("tls: the peer closed the connection during the handshake" +
                        (details.empty() ? std::string() : " (" + details + ")"));
        }
        return fail("tls: handshake failed: " + openssl_errors());
    }

    X509* peer = SSL_get0_peer_certificate(impl->ssl.get());
    if (!peer) return fail("tls: the peer sent no certificate");
    if (SSL_version(impl->ssl.get()) != TLS1_3_VERSION) return fail("tls: not TLS 1.3");

    auto stream = std::unique_ptr<Stream>(new Stream(std::move(impl)));
    stream->peer_id_ = spki_id(peer);
    stream->peer_name_ = common_name(peer);
    return stream;
}

bool Stream::send_all(const char* data, std::size_t size) {
    std::lock_guard writer(impl_->write_mutex);
    std::size_t sent = 0;
    while (sent < size) {
        if (impl_->aborted) return false;
        std::size_t written = 0;
        int r = 0, e = SSL_ERROR_NONE;
        {
            std::lock_guard lock(impl_->ssl_mutex);
            ERR_clear_error();
            r = SSL_write_ex(impl_->ssl.get(), data + sent, size - sent, &written);
            if (r != 1) e = SSL_get_error(impl_->ssl.get(), r);
        }
        if (r == 1) {
            sent += written;
        } else if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) {
            if (wait_socket(impl_->sock, e == SSL_ERROR_WANT_WRITE, kForever, impl_->aborted) != Wait::Ready) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

int Stream::recv_exact(char* data, std::size_t size) {
    std::size_t got = 0;
    while (got < size) {
        if (impl_->aborted) return WSAECONNABORTED;
        std::size_t n = 0;
        int r = 0, e = SSL_ERROR_NONE, sys = 0;
        {
            std::lock_guard lock(impl_->ssl_mutex);
            ERR_clear_error();
            r = SSL_read_ex(impl_->ssl.get(), data + got, size - got, &n);
            if (r != 1) {
                e = SSL_get_error(impl_->ssl.get(), r);
                if (e == SSL_ERROR_SYSCALL) sys = WSAGetLastError();
            }
        }
        if (r == 1) {
            got += n;
            continue;
        }
        switch (e) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE: {
            const auto timeout = std::chrono::milliseconds(impl_->receive_timeout_ms.load());
            const auto deadline = timeout.count() > 0 ? Clock::now() + timeout : kForever;
            switch (wait_socket(impl_->sock, e == SSL_ERROR_WANT_WRITE, deadline, impl_->aborted)) {
            case Wait::Ready: continue;
            case Wait::Aborted: return WSAECONNABORTED;
            case Wait::Timeout: return WSAETIMEDOUT;
            default: return impl_->aborted ? WSAECONNABORTED : WSAECONNRESET;
            }
        }
        case SSL_ERROR_SYSCALL: return impl_->aborted ? WSAECONNABORTED : (sys != 0 ? sys : WSAECONNRESET);
        default: return impl_->aborted ? WSAECONNABORTED : WSAECONNRESET;  // close_notify or a TLS error
        }
    }
    return 0;
}

void Stream::set_receive_timeout(std::chrono::milliseconds timeout) {
    impl_->receive_timeout_ms = timeout.count();
}

void Stream::abort() {
    impl_->aborted = true;
    net::detail::abort_socket(impl_->sock);
}

std::vector<std::uint8_t> Stream::export_keying_material(std::string_view label, std::span<const std::uint8_t> context,
                                                         std::size_t length) const {
    std::vector<std::uint8_t> out(length);
    std::lock_guard lock(impl_->ssl_mutex);
    if (SSL_export_keying_material(impl_->ssl.get(), out.data(), out.size(), label.data(), label.size(), context.data(),
                                   context.size(), 1) != 1) {
        ERR_clear_error();
        return {};
    }
    return out;
}

}  // namespace kvm::tls
