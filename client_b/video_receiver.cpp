#include "video_receiver.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <cstring>
#include <format>
#include <memory>
#include <stdexcept>
#include <utility>

#include "jpeg_decoder.h"
#include "kvm/log.h"
#include "kvm/net.h"

namespace kvm {

namespace video_wire {

namespace {

constexpr std::size_t kPrefixBytes = 8;  // u32 header_len + u32 payload_len

bool send_all(SOCKET s, const char* data, std::size_t size) {
    while (size > 0) {
        const int sent = ::send(s, data, static_cast<int>(size), 0);
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
        const int got = ::recv(s, data, static_cast<int>(size), 0);
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

void set_nodelay(SOCKET s) {
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

}  // namespace

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        const SOCKET old = sock_.exchange(other.sock_.exchange(INVALID_SOCKET));
        if (old != INVALID_SOCKET) {
            closesocket(old);
        }
    }
    return *this;
}

void Socket::close() {
    const SOCKET s = sock_.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET) {
        closesocket(s);
    }
}

Socket listen_tcp(std::uint16_t port) {
    net::ensure_winsock();
    Socket s(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!s.valid()) {
        throw std::runtime_error("socket() failed: " + std::to_string(WSAGetLastError()));
    }
    BOOL exclusive = TRUE;
    setsockopt(s.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(s.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        ::listen(s.get(), SOMAXCONN) == SOCKET_ERROR) {
        throw std::runtime_error("cannot listen on port " + std::to_string(port) + ": error " +
                                 std::to_string(WSAGetLastError()));
    }
    return s;
}

Socket connect_tcp(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout, int* error) {
    net::ensure_winsock();
    *error = 0;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* found = nullptr;
    if (const int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found); rc != 0) {
        *error = rc;
        return {};
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(found, &freeaddrinfo);

    Socket s(::socket(found->ai_family, found->ai_socktype, found->ai_protocol));
    if (!s.valid()) {
        *error = WSAGetLastError();
        return {};
    }

    // Non-blocking connect + select() so an unreachable host fails after `timeout`, not ~21 s.
    u_long non_blocking = 1;
    ioctlsocket(s.get(), FIONBIO, &non_blocking);
    if (::connect(s.get(), found->ai_addr, static_cast<int>(found->ai_addrlen)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            fd_set writable;
            fd_set failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(s.get(), &writable);
            FD_SET(s.get(), &failed);
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
                getsockopt(s.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &so_len);
                err = so_error;
            }
        }
        if (err != 0) {
            *error = err;
            return {};
        }
    }
    non_blocking = 0;
    ioctlsocket(s.get(), FIONBIO, &non_blocking);
    set_nodelay(s.get());
    return s;
}

bool send_frame(SOCKET s, const proto::FrameHeader& header, std::span<const std::uint8_t> payload) {
    const std::string json = proto::encode(header).dump();
    const u_long header_len = htonl(static_cast<u_long>(json.size()));
    const u_long payload_len = htonl(static_cast<u_long>(payload.size()));

    // One buffer -> one send(): with TCP_NODELAY, separate small sends would go out as extra packets.
    std::vector<char> buf(kPrefixBytes + json.size() + payload.size());
    std::memcpy(buf.data(), &header_len, 4);
    std::memcpy(buf.data() + 4, &payload_len, 4);
    std::memcpy(buf.data() + kPrefixBytes, json.data(), json.size());
    if (!payload.empty()) {
        std::memcpy(buf.data() + kPrefixBytes + json.size(), payload.data(), payload.size());
    }
    return send_all(s, buf.data(), buf.size());
}

int recv_frame(SOCKET s, Frame& out) {
    std::array<char, kPrefixBytes> prefix{};
    if (const int err = recv_exact(s, prefix.data(), prefix.size()); err != 0) {
        return err;
    }
    u_long header_len = 0;
    u_long payload_len = 0;
    std::memcpy(&header_len, prefix.data(), 4);
    std::memcpy(&payload_len, prefix.data() + 4, 4);
    header_len = ntohl(header_len);
    payload_len = ntohl(payload_len);
    if (header_len == 0 || header_len > proto::kMaxControlMessageBytes || payload_len > proto::kMaxFramePayloadBytes) {
        log::warn("video: bad frame sizes (header {} B, payload {} B)", header_len, payload_len);
        return WSAEMSGSIZE;
    }

    std::string json(header_len, '\0');
    if (const int err = recv_exact(s, json.data(), json.size()); err != 0) {
        return err;
    }
    const auto j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || j.value("type", "") != proto::FrameHeader::kType) {
        log::warn("video: frame header is not a 'frame' message");
        return WSAEINVAL;
    }
    try {
        out.header = j.get<proto::FrameHeader>();
    } catch (const nlohmann::json::exception&) {
        log::warn("video: malformed frame header");
        return WSAEINVAL;
    }

    out.payload.resize(payload_len);
    return recv_exact(s, reinterpret_cast<char*>(out.payload.data()), out.payload.size());
}

}  // namespace video_wire

// ---------------------------------------------------------------------------------------------

namespace {

constexpr auto kConnectTimeout = std::chrono::seconds(2);
constexpr auto kReconnectDelay = std::chrono::seconds(1);

std::wstring widen(const std::string& s) {
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

}  // namespace

VideoReceiver::VideoReceiver(std::string host, std::uint16_t port, ViewerWindow& viewer)
    : host_(std::move(host)), port_(port), viewer_(viewer) {
    receive_thread_ = std::jthread([this](std::stop_token stop) { receive_loop(stop); });
    decode_thread_ = std::jthread([this](std::stop_token stop) { decode_loop(stop); });
}

VideoReceiver::~VideoReceiver() {
    // Order matters: request_stop() first, then close the socket. receive_loop re-checks the stop
    // flag after storing a new socket, so a connect that races with shutdown cannot block forever.
    receive_thread_.request_stop();
    sock_.close();
    decode_thread_.request_stop();
    latest_.close();
    // jthread members join here.
}

void VideoReceiver::show_status(std::wstring text) {
    viewer_.set_status(std::move(text));
}

void VideoReceiver::receive_loop(std::stop_token stop) {
    bool failure_logged = false;
    while (!stop.stop_requested()) {
        int err = 0;
        video_wire::Socket s = video_wire::connect_tcp(host_, port_, kConnectTimeout, &err);
        if (!s.valid()) {
            if (!failure_logged) {
                log::warn("video: cannot connect to {}:{} (error {}), retrying every {} s", host_, port_, err,
                          kReconnectDelay.count());
                show_status(std::format(L"Connecting to {}:{}...", widen(host_), port_));
                failure_logged = true;
            }
            const auto until = std::chrono::steady_clock::now() + kReconnectDelay;
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }

        sock_ = std::move(s);
        if (stop.stop_requested()) {
            break;
        }
        failure_logged = false;
        log::info("video: connected to {}:{}", host_, port_);
        show_status(L"Connected, waiting for frames...");

        while (true) {
            video_wire::Frame frame;
            if (const int rc = video_wire::recv_frame(sock_.get(), frame); rc != 0) {
                if (!stop.stop_requested()) {
                    log::warn("video: connection lost (error {}), reconnecting", rc);
                    show_status(L"Disconnected, reconnecting...");
                }
                break;
            }
            latest_.put(std::move(frame));
        }
        sock_.close();
    }
}

void VideoReceiver::decode_loop(std::stop_token stop) {
    JpegDecoder decoder;
    BgraImage spare;  // reused: present() hands back the previous frame's buffer
    bool codec_warned = false;
    std::uint64_t decoded_frames = 0;

    while (!stop.stop_requested()) {
        auto frame = latest_.take();
        if (!frame) {
            break;  // closed
        }
        if (frame->header.codec != "jpeg") {
            if (!codec_warned) {
                log::warn("video: unsupported codec '{}', frames ignored", frame->header.codec);
                codec_warned = true;
            }
            continue;
        }

        const std::int64_t t0 = steady_now_us();
        if (!decoder.decode(frame->payload, spare)) {
            log::warn("video: frame {} dropped: {}", frame->header.seq, decoder.last_error());
            continue;
        }
        const std::int64_t t1 = steady_now_us();
        viewer_.present(spare);
        ++decoded_frames;

        // Capture on A -> decoded on B. t_capture_us is A's clock; subtracting the offset maps it to ours.
        const std::int64_t captured_local = frame->header.t_capture_us - clock_offset_us_;
        const double latency_ms = static_cast<double>(t1 - captured_local) / 1000.0;
        const double decode_ms = static_cast<double>(t1 - t0) / 1000.0;
        show_status(std::format(L"latency {:.0f} ms   decode {:.1f} ms   {} KB   dropped {}", latency_ms, decode_ms,
                                frame->payload.size() / 1024, latest_.overwritten()));
    }
    log::info("video: decode thread stopped after {} frames", decoded_frames);
}

}  // namespace kvm
