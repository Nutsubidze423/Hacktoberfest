// SPDX-License-Identifier: Apache-2.0

#include "kvm/net.h"

#include <winsock2.h>
#include <ws2tcpip.h>

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

// recv() fails with WSAETIMEDOUT when nothing arrives for this long. Pings every kPingInterval
// guarantee traffic on a healthy link.
void set_receive_timeout(SOCKET s) {
    const DWORD ms = static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(kDeadAfter).count());
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

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
        const std::string err = last_error_text();
        closesocket(s);
        throw std::runtime_error("cannot listen on port " + std::to_string(port) + ": " + err);
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

Connection::Connection(SOCKET sock, std::string peer) : sock_(sock), peer_(std::move(peer)) {
    set_nodelay(sock_);
    set_receive_timeout(sock_);
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
    closesocket(sock_);
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
    if (!send_all(sock_, frame.data(), frame.size())) {
        if (open_) {
            log::warn("net: send to {} failed: {}", peer_, last_error_text());
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
        // abort (not closesocket) wakes a blocked recv()/send() without invalidating the handle
        // that other threads may still be using; the destructor closes it after joining.
        abort_socket(sock_);
        stop_.request_stop();
    }
}

void Connection::receive_loop() {
    std::vector<char> body;
    while (open_) {
        u_long net_len = 0;
        int err = recv_exact(sock_, reinterpret_cast<char*>(&net_len), kHeaderBytes);
        if (err == 0) {
            const std::uint32_t len = ntohl(net_len);
            if (len == 0 || len > proto::kMaxControlMessageBytes) {
                log::error("net: bad message length {} from {}", len, peer_);
                break;
            }
            body.resize(len);
            err = recv_exact(sock_, body.data(), len);
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

std::unique_ptr<Connection> Server::accept() {
    std::string peer;
    const SOCKET s = accept_on(listen_sock_, peer);
    return s == INVALID_SOCKET ? nullptr : std::make_unique<Connection>(s, std::move(peer));
}

void Server::stop() {
    close_listener(listen_sock_);
}

std::unique_ptr<Connection> connect(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout) {
    const SOCKET s = connect_to(host, port, timeout);
    return s == INVALID_SOCKET ? nullptr : std::make_unique<Connection>(s, host + ":" + std::to_string(port));
}

}  // namespace kvm::net
