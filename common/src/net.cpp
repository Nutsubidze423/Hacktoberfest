// SPDX-License-Identifier: Apache-2.0

#include "kvm/net.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "kvm/log.h"
#include "socket_util.h"

namespace kvm {

std::int64_t steady_now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace kvm

namespace kvm::net {

using namespace detail;

namespace {

constexpr std::size_t kHeaderBytes = 4;

std::string peer_name(const sockaddr* addr, int len) {
    std::array<char, NI_MAXHOST> host{};
    std::array<char, NI_MAXSERV> port{};
    if (getnameinfo(addr, len, host.data(), static_cast<DWORD>(host.size()), port.data(),
                    static_cast<DWORD>(port.size()), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return "?";
    }
    return std::string(host.data()) + ":" + port.data();
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Socket helpers (socket_util.h)

namespace detail {

std::string socket_error_text(int code) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   static_cast<DWORD>(code), 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::string text = std::to_string(code);
    if (buf) {
        std::wstring_view w(buf);
        while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L'.')) {
            w.remove_suffix(1);
        }
        const int len =
            WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string narrow(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), narrow.data(), len, nullptr, nullptr);
        text += " (" + narrow + ")";
        LocalFree(buf);
    }
    return text;
}

std::string last_error_text() {
    return socket_error_text(WSAGetLastError());
}

namespace {

// The program listening on TCP `port` (IPv4), as "name.exe (pid N)", or empty if none is.
std::string listener_of(std::uint16_t port) {
    std::vector<std::uint8_t> buffer(16 * 1024);
    ULONG size = static_cast<ULONG>(buffer.size());
    DWORD result = 0;
    while ((result = GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0)) ==
           ERROR_INSUFFICIENT_BUFFER) {
        buffer.resize(size);
    }
    if (result != NO_ERROR) return {};
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        if (ntohs(static_cast<u_short>(table->table[i].dwLocalPort)) != port) continue;
        const DWORD pid = table->table[i].dwOwningPid;
        std::string name = "process";
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t path[MAX_PATH]{};
            DWORD length = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, path, &length)) {
                std::wstring_view file(path, length);
                if (const auto slash = file.find_last_of(L'\\'); slash != std::wstring_view::npos) {
                    file.remove_prefix(slash + 1);
                }
                const int len = WideCharToMultiByte(CP_UTF8, 0, file.data(), static_cast<int>(file.size()), nullptr, 0,
                                                    nullptr, nullptr);
                name.assign(len, '\0');
                WideCharToMultiByte(CP_UTF8, 0, file.data(), static_cast<int>(file.size()), name.data(), len, nullptr,
                                    nullptr);
            }
            CloseHandle(process);
        }
        return name + " (pid " + std::to_string(pid) + ")";
    }
    return {};
}

// Why `port` can't be listened on, in words a user can act on.
std::string listen_error_text(std::uint16_t port, int code) {
    if (code == WSAEADDRINUSE || code == WSAEACCES) {
        if (const std::string owner = listener_of(port); !owner.empty()) {
            return "it is in use by " + owner +
                   ". Close that program, or give Sarke other ports in config.json "
                   "(control_port, video_port, file_port; the same on both PCs)";
        }
        if (code == WSAEACCES) {
            return "Windows reserved it (Hyper-V, WSL and Docker reserve port ranges: see `netsh interface ipv4 "
                   "show excludedportrange protocol=tcp`). Give Sarke free ports in config.json (control_port, "
                   "video_port, file_port; the same on both PCs)";
        }
    }
    return socket_error_text(code);
}

}  // namespace

void set_nodelay(SOCKET s) {
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

void abort_socket(SOCKET s) {
    shutdown(s, SD_BOTH);
    CancelIoEx(reinterpret_cast<HANDLE>(s), nullptr);
}

bool send_all(SOCKET s, const char* data, std::size_t size) {
    while (size > 0) {
        const int sent = ::send(s, data, static_cast<int>(std::min<std::size_t>(size, INT32_MAX)), 0);
        if (sent == SOCKET_ERROR) {
            return false;
        }
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

int recv_exact(SOCKET s, char* data, std::size_t size) {
    while (size > 0) {
        const int got = ::recv(s, data, static_cast<int>(std::min<std::size_t>(size, INT32_MAX)), 0);
        if (got == 0) {
            return WSAECONNRESET;
        }
        if (got == SOCKET_ERROR) {
            return WSAGetLastError();
        }
        data += got;
        size -= static_cast<std::size_t>(got);
    }
    return 0;
}

SOCKET listen_on(std::uint16_t port) {
    ensure_winsock();
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        throw std::runtime_error("socket() failed: " + last_error_text());
    }
    // Refuse to share the port with another process (e.g. a second kvm_host left running).
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        ::listen(s, SOMAXCONN) == SOCKET_ERROR) {
        const int code = WSAGetLastError();
        closesocket(s);
        throw std::runtime_error("cannot listen on port " + std::to_string(port) + ": " +
                                 listen_error_text(port, code));
    }
    return s;
}

SOCKET accept_on(const std::atomic<SOCKET>& listen_sock, std::string& peer) {
    const SOCKET listening = listen_sock;
    if (listening == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    sockaddr_storage addr{};
    int len = sizeof(addr);
    const SOCKET s = ::accept(listening, reinterpret_cast<sockaddr*>(&addr), &len);
    if (s == INVALID_SOCKET) {
        if (listen_sock != INVALID_SOCKET) {
            log::warn("net: accept failed: {}", last_error_text());
        }
        return INVALID_SOCKET;
    }
    set_nodelay(s);
    peer = peer_name(reinterpret_cast<sockaddr*>(&addr), len);
    return s;
}

void close_listener(std::atomic<SOCKET>& listen_sock) {
    const SOCKET s = listen_sock.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET) {
        closesocket(s);  // makes a blocked accept() return INVALID_SOCKET
    }
}

SOCKET connect_to(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout) {
    ensure_winsock();

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* found = nullptr;
    if (const int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found); rc != 0) {
        log::warn("net: cannot resolve {}: {}", host, socket_error_text(rc));
        return INVALID_SOCKET;
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(found, &freeaddrinfo);

    const SOCKET s = ::socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    if (s == INVALID_SOCKET) {
        log::warn("net: socket() failed: {}", last_error_text());
        return INVALID_SOCKET;
    }

    // Non-blocking connect + select() so an unreachable host fails after `timeout`,
    // not after Windows' ~21 s default.
    u_long non_blocking = 1;
    ioctlsocket(s, FIONBIO, &non_blocking);
    int err = 0;
    if (::connect(s, found->ai_addr, static_cast<int>(found->ai_addrlen)) == SOCKET_ERROR) {
        err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            fd_set writable;
            fd_set failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(s, &writable);
            FD_SET(s, &failed);
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(timeout).count();
            timeval tv{.tv_sec = static_cast<long>(us / 1'000'000), .tv_usec = static_cast<long>(us % 1'000'000)};
            const int ready = select(0, nullptr, &writable, &failed, &tv);
            if (ready == 0) {
                err = WSAETIMEDOUT;
            } else if (ready == SOCKET_ERROR) {
                err = WSAGetLastError();
            } else {
                int so_error = 0;
                int so_len = sizeof(so_error);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &so_len);
                err = so_error;
            }
        }
    }
    if (err != 0) {
        log::warn("net: connect to {}:{} failed: {}", host, port, socket_error_text(err));
        closesocket(s);
        return INVALID_SOCKET;
    }
    non_blocking = 0;
    ioctlsocket(s, FIONBIO, &non_blocking);
    set_nodelay(s);
    return s;
}

}  // namespace detail

void ensure_winsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        if (const int rc = WSAStartup(MAKEWORD(2, 2), &data); rc != 0) {
            throw std::runtime_error("WSAStartup failed: " + socket_error_text(rc));
        }
        // No WSACleanup: Winsock lives until process exit, which also closes every socket.
    });
}

// ---------------------------------------------------------------------------------------------
// Connection

Connection::Connection(Handshaken connection) : tls_(std::move(connection.stream)), peer_(std::move(connection.peer)) {
    tls_->set_receive_timeout(kDeadAfter);
    ping_thread_ = std::jthread([this] { ping_loop(stop_.get_token()); });
    receive_thread_ = std::jthread([this] { receive_loop(); });
}

Connection::~Connection() {
    close();
    if (ping_thread_.joinable()) {
        ping_thread_.join();
    }
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }
    // tls_ closes the socket when it goes.
}

bool Connection::send_json(const nlohmann::json& j) {
    if (!open_) {
        return false;
    }
    const std::string body = j.dump();
    if (body.size() > proto::kMaxControlMessageBytes) {
        log::error("net: message too large ({} bytes), not sent", body.size());
        return false;
    }
    std::string frame(kHeaderBytes + body.size(), '\0');
    const u_long len = htonl(static_cast<u_long>(body.size()));
    std::memcpy(frame.data(), &len, kHeaderBytes);
    std::memcpy(frame.data() + kHeaderBytes, body.data(), body.size());

    std::lock_guard lock(send_mutex_);
    if (!tls_->send_all(frame.data(), frame.size())) {
        if (open_) {
            log::warn("net: send to {} failed", peer_);
        }
        close();
        return false;
    }
    return true;
}

LinkStats Connection::stats() const {
    return {.rtt_us = rtt_us_, .clock_offset_us = clock_offset_us_};
}

void Connection::close() {
    if (open_.exchange(false)) {
        // abort (not close) wakes a blocked receive/send without invalidating the socket that other
        // threads may still be using; tls_ closes it after the threads are joined.
        tls_->abort();
        stop_.request_stop();
    }
}

void Connection::receive_loop() {
    std::vector<char> body;
    while (open_) {
        u_long net_len = 0;
        int err = tls_->recv_exact(reinterpret_cast<char*>(&net_len), kHeaderBytes);
        if (err == 0) {
            const std::uint32_t len = ntohl(net_len);
            if (len == 0 || len > proto::kMaxControlMessageBytes) {
                log::error("net: bad message length {} from {}", len, peer_);
                break;
            }
            body.resize(len);
            err = tls_->recv_exact(body.data(), len);
        }
        if (err != 0) {
            if (!open_) {
                break;  // closed locally
            }
            if (err == WSAETIMEDOUT) {
                log::warn("net: no data from {} for {} s, dropping connection", peer_,
                          std::chrono::duration_cast<std::chrono::seconds>(kDeadAfter).count());
            } else if (err == WSAECONNRESET || err == WSAECONNABORTED) {
                log::info("net: {} disconnected", peer_);
            } else {
                log::warn("net: receive from {} failed: {}", peer_, socket_error_text(err));
            }
            break;
        }

        const auto j = nlohmann::json::parse(body.begin(), body.end(), nullptr, /*allow_exceptions=*/false);
        const auto msg = j.is_discarded() ? std::nullopt : proto::decode(j);
        if (!msg) {
            log::warn("net: ignoring undecodable message from {}: {}", peer_,
                      std::string_view(body.data(), std::min<std::size_t>(body.size(), 200)));
            continue;
        }
        if (const auto* ping = std::get_if<proto::Ping>(&*msg)) {
            send(proto::Pong{.t = ping->t, .t_peer = steady_now_us()});
        } else if (const auto* pong = std::get_if<proto::Pong>(&*msg)) {
            handle_pong(*pong);
        } else {
            incoming_.push(*msg);
        }
    }
    close();
    incoming_.close();
}

void Connection::ping_loop(std::stop_token stop) {
    std::mutex m;
    std::condition_variable_any cv;
    std::unique_lock lock(m);
    while (!stop.stop_requested() && open_) {
        send(proto::Ping{.t = steady_now_us()});
        cv.wait_for(lock, stop, kPingInterval, [] { return false; });
    }
}

void Connection::handle_pong(const proto::Pong& pong) {
    const std::int64_t now = steady_now_us();
    const std::int64_t rtt = now - pong.t;
    if (rtt < 0) {
        return;  // not one of our pings
    }
    rtt_us_ = rtt;

    // The peer stamped t_peer roughly half a round trip after we sent the ping. That only holds
    // when both legs took equally long, which is most likely for the fastest ping, so (like NTP)
    // the offset comes from the lowest-RTT sample among the last kOffsetSamples.
    offset_samples_[offset_sample_count_++ % offset_samples_.size()] = {rtt, pong.t_peer - (pong.t + rtt / 2)};
    const auto filled = std::min(offset_sample_count_, offset_samples_.size());
    const auto best = std::min_element(offset_samples_.begin(), offset_samples_.begin() + filled,
                                       [](const auto& a, const auto& b) { return a.rtt_us < b.rtt_us; });
    clock_offset_us_ = best->offset_us;
}

// ---------------------------------------------------------------------------------------------
// Server / connect

Server::Server(std::uint16_t port) : listen_sock_(listen_on(port)) {}

Server::~Server() {
    stop();
}

std::optional<Handshaken> Server::accept(const tls::Credentials& me, std::chrono::milliseconds handshake_timeout) {
    for (;;) {
        std::string peer;
        const SOCKET s = accept_on(listen_sock_, peer);
        if (s == INVALID_SOCKET) return std::nullopt;  // stop()
        set_nodelay(s);
        std::string why;
        if (auto stream = tls::Stream::handshake(s, tls::Role::Server, me, handshake_timeout, why)) {
            return Handshaken{std::move(stream), std::move(peer)};
        }
        log::warn("net: {}: {}", peer, why);  // the socket is closed; wait for the next client
    }
}

void Server::stop() {
    close_listener(listen_sock_);
}

std::optional<Handshaken> connect(const std::string& host, std::uint16_t port, const tls::Credentials& me,
                                  std::chrono::milliseconds timeout) {
    const SOCKET s = connect_to(host, port, timeout);
    if (s == INVALID_SOCKET) return std::nullopt;
    const std::string peer = host + ":" + std::to_string(port);
    std::string why;
    auto stream = tls::Stream::handshake(s, tls::Role::Client, me, kHandshakeTimeout, why);
    if (!stream) {
        log::warn("net: {}: {}", peer, why);
        return std::nullopt;
    }
    return Handshaken{std::move(stream), peer};
}

// ---------------------------------------------------------------------------------------------
// Side channels

namespace {

constexpr std::string_view kChannelHello = "channel_hello";
constexpr std::uint32_t kMaxChannelHelloBytes = 1024;

// [u32 header_len][u32 payload_len = 0][{"type":"channel_hello","token":"..."}]: the side channels'
// own framing, so the first frame reads like any other.
bool send_channel_hello(tls::Stream& stream, std::string_view token) {
    const std::string json = nlohmann::json{{"type", kChannelHello}, {"token", token}}.dump();
    std::string frame(8 + json.size(), '\0');
    const u_long lens[2] = {htonl(static_cast<u_long>(json.size())), 0};
    std::memcpy(frame.data(), lens, 8);
    std::memcpy(frame.data() + 8, json.data(), json.size());
    return stream.send_all(frame.data(), frame.size());
}

std::optional<std::string> recv_channel_hello(tls::Stream& stream, std::chrono::milliseconds timeout) {
    stream.set_receive_timeout(timeout);
    std::array<u_long, 2> lens{};
    std::optional<std::string> token;
    if (stream.recv_exact(reinterpret_cast<char*>(lens.data()), 8) == 0) {
        const std::uint32_t header_len = ntohl(lens[0]);
        if (header_len > 0 && header_len <= kMaxChannelHelloBytes && ntohl(lens[1]) == 0) {
            std::string json(header_len, '\0');
            if (stream.recv_exact(json.data(), header_len) == 0) {
                const auto j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
                if (j.is_object() && j.value("type", std::string()) == kChannelHello && j.contains("token") &&
                    j["token"].is_string()) {
                    token = j["token"].get<std::string>();
                }
            }
        }
    }
    stream.set_receive_timeout(std::chrono::milliseconds(0));  // side channels may be idle for long
    return token;
}

}  // namespace

std::string SessionKey::open(const tls::DeviceId& peer) {
    std::lock_guard lock(mutex_);
    peer_ = peer;
    token_ = tls::random_hex(32);
    return token_;
}

void SessionKey::close() {
    std::lock_guard lock(mutex_);
    peer_.reset();
    token_.clear();
}

bool SessionKey::admit(const tls::DeviceId& peer, std::string_view token, const std::function<void()>& install) const {
    std::lock_guard lock(mutex_);
    if (!peer_ || *peer_ != peer || !tls::equal_secret(token_, token)) return false;
    if (install) install();
    return true;
}

bool admit_side_channel(tls::Stream& stream, const SessionKey& session, std::string_view what, std::string_view peer,
                        const std::function<void()>& install, std::chrono::milliseconds timeout) {
    const auto token = recv_channel_hello(stream, timeout);
    if (!token) {
        log::warn("{}: {} sent no channel_hello - refused", what, peer);
        return false;
    }
    const auto key = stream.peer_id();  // `install` may move the stream away
    if (!session.admit(key, *token, install)) {
        log::warn("{}: {} (key {}) isn't part of the live session - refused", what, peer, tls::short_id(key));
        return false;
    }
    return true;
}

std::optional<Handshaken> connect_side_channel(const std::string& host, std::uint16_t port, const tls::Credentials& me,
                                               const tls::DeviceId& host_id, std::string_view token,
                                               std::chrono::milliseconds timeout) {
    auto pending = connect(host, port, me, timeout);
    if (!pending) return std::nullopt;
    if (pending->stream->peer_id() != host_id) {
        log::warn("net: {} answered with the key {}, not the host's - closed", pending->peer,
                  tls::short_id(pending->stream->peer_id()));
        return std::nullopt;
    }
    if (!send_channel_hello(*pending->stream, token)) {
        log::warn("net: {}: sending channel_hello failed", pending->peer);
        return std::nullopt;
    }
    return pending;
}

}  // namespace kvm::net
