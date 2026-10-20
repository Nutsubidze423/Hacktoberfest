// SPDX-License-Identifier: Apache-2.0

#include "kvm/video.h"

#include <winsock2.h>

#include <array>
#include <cstring>
#include <string_view>

#include "kvm/log.h"
#include "socket_util.h"

namespace kvm::net {

using namespace detail;

namespace {

constexpr std::size_t kPrefixBytes = 8;  // u32 header_len + u32 payload_len

}  // namespace

FrameChannel::FrameChannel(Handshaken connection)
    : tls_(std::move(connection.stream)), peer_(std::move(connection.peer)) {}

FrameChannel::~FrameChannel() {
    close();  // tls_ closes the socket when it goes
}

void FrameChannel::close() {
    if (open_.exchange(false)) {
        tls_->abort();  // wakes blocked send/recv at once; the destructor closes the socket
    }
}

bool FrameChannel::send_frame(const proto::FrameHeader& header, std::span<const std::uint8_t> payload) {
    if (!open_) {
        return false;
    }
    const std::string header_json = proto::encode(header).dump();
    if (header_json.size() > proto::kMaxControlMessageBytes || payload.size() > proto::kMaxFramePayloadBytes) {
        log::error("video: frame too large (header {} B, payload {} B), not sent", header_json.size(), payload.size());
        return false;
    }

    // Prefix + header in one buffer, payload sent straight from the caller's memory (no copy).
    std::string head(kPrefixBytes + header_json.size(), '\0');
    const u_long lens[2] = {htonl(static_cast<u_long>(header_json.size())), htonl(static_cast<u_long>(payload.size()))};
    std::memcpy(head.data(), lens, kPrefixBytes);
    std::memcpy(head.data() + kPrefixBytes, header_json.data(), header_json.size());

    std::lock_guard lock(send_mutex_);
    const bool ok = tls_->send_all(head.data(), head.size()) &&
                    tls_->send_all(reinterpret_cast<const char*>(payload.data()), payload.size());
    if (!ok) {
        if (open_) {
            log::warn("video: send to {} failed", peer_);
        }
        close();
    }
    return ok;
}

std::optional<Frame> FrameChannel::recv_frame() {
    const auto fail = [this](int err) -> std::optional<Frame> {
        if (open_) {
            if (err == WSAECONNRESET || err == WSAECONNABORTED) {
                log::info("video: {} disconnected", peer_);
            } else if (err != 0) {
                log::warn("video: receive from {} failed: {}", peer_, socket_error_text(err));
            }
        }
        close();
        return std::nullopt;
    };

    if (!open_) {
        return std::nullopt;
    }
    std::array<u_long, 2> lens{};
    if (const int err = tls_->recv_exact(reinterpret_cast<char*>(lens.data()), kPrefixBytes); err != 0) {
        return fail(err);
    }
    const std::uint32_t header_len = ntohl(lens[0]);
    const std::uint32_t payload_len = ntohl(lens[1]);
    if (header_len == 0 || header_len > proto::kMaxControlMessageBytes || payload_len > proto::kMaxFramePayloadBytes) {
        log::error("video: bad frame lengths from {} (header {}, payload {})", peer_, header_len, payload_len);
        return fail(0);
    }

    std::string header_json(header_len, '\0');
    if (const int err = tls_->recv_exact(header_json.data(), header_len); err != 0) {
        return fail(err);
    }
    const auto j = nlohmann::json::parse(header_json, nullptr, /*allow_exceptions=*/false);
    const auto type = j.is_object() ? j.find("type") : j.end();
    if (j.is_discarded() || type == j.end() || !type->is_string() ||
        type->get_ref<const std::string&>() != proto::FrameHeader::kType) {
        log::error("video: bad frame header from {}: {}", peer_, std::string_view(header_json).substr(0, 200));
        return fail(0);
    }

    Frame frame;
    try {
        frame.header = j.get<proto::FrameHeader>();
    } catch (const nlohmann::json::exception& e) {
        log::error("video: bad frame header from {}: {}", peer_, e.what());
        return fail(0);
    }
    frame.payload.resize(payload_len);
    if (const int err = tls_->recv_exact(reinterpret_cast<char*>(frame.payload.data()), payload_len); err != 0) {
        return fail(err);
    }
    return frame;
}

VideoServer::VideoServer(std::uint16_t port) : server_(port) {}

VideoServer::~VideoServer() {
    stop();
}

std::optional<Handshaken> VideoServer::accept(const tls::Credentials& me) {
    return server_.accept(me);
}

void VideoServer::stop() {
    server_.stop();
}

std::unique_ptr<FrameChannel> connect_video(const std::string& host, std::uint16_t port, const tls::Credentials& me,
                                            const tls::DeviceId& host_id, std::string_view token,
                                            std::chrono::milliseconds timeout) {
    auto pending = connect_side_channel(host, port, me, host_id, token, timeout);
    return pending ? std::make_unique<FrameChannel>(std::move(*pending)) : nullptr;
}

}  // namespace kvm::net
