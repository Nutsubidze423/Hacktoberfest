#include "video_receiver.h"

#include <chrono>
#include <utility>

#include "jpeg_decoder.h"
#include "kvm/log.h"
#include "kvm/net.h"

namespace kvm {

namespace {

constexpr auto kConnectTimeout = std::chrono::seconds(2);
constexpr auto kReconnectDelay = std::chrono::seconds(1);

}  // namespace

VideoReceiver::VideoReceiver(std::string host, std::uint16_t port, ViewerWindow& viewer)
    : host_(std::move(host)), port_(port), viewer_(viewer) {
    viewer_.set_endpoint(host_, port_);
    viewer_.set_reconnects(true);
    receive_thread_ = std::jthread([this](std::stop_token stop) { receive_loop(stop); });
    decode_thread_ = std::jthread([this](std::stop_token stop) { decode_loop(stop); });
}

VideoReceiver::~VideoReceiver() {
    // request_stop() first, then close the channel: receive_loop re-checks the stop flag after it
    // stores a new channel, so a connect that races with shutdown cannot block in recv_frame().
    receive_thread_.request_stop();
    {
        std::lock_guard lock(channel_mutex_);
        if (channel_) {
            channel_->close();
        }
    }
    decode_thread_.request_stop();
    latest_.close();
    // jthread members join here; the channel is destroyed after its reader has finished.
}

void VideoReceiver::receive_loop(std::stop_token stop) {
    bool lost = false;  // after a disconnect the viewer keeps showing LOST until we are back
    while (!stop.stop_requested()) {
        if (!lost) {
            viewer_.set_link_state(LinkState::Connecting);
        }
        auto channel = net::connect_video(host_, port_, kConnectTimeout);  // logs failures itself
        if (!channel) {
            if (!lost) {
                viewer_.set_link_state(LinkState::Waiting);
            }
            const auto until = std::chrono::steady_clock::now() + kReconnectDelay;
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }

        net::FrameChannel* ch = channel.get();
        {
            std::lock_guard lock(channel_mutex_);
            channel_ = std::move(channel);
        }
        if (stop.stop_requested()) {
            break;
        }
        lost = false;
        log::info("video: connected to {}", ch->peer());
        viewer_.set_link_state(LinkState::Linked);

        while (auto frame = ch->recv_frame()) {
            viewer_.count_payload_bytes(frame->payload.size());
            latest_.put(std::move(*frame));
        }
        if (!stop.stop_requested()) {
            log::warn("video: connection to {} lost, reconnecting", ch->peer());
            viewer_.set_link_state(LinkState::Lost);
            lost = true;
        }
        std::lock_guard lock(channel_mutex_);
        channel_.reset();  // nobody is inside recv_frame() any more
    }
}

void VideoReceiver::decode_loop(std::stop_token stop) {
    JpegDecoder decoder;
    BgraImage spare;  // reused: present() hands back a spare buffer
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
        if (!decoder.decode(frame->payload, spare)) {
            log::warn("video: frame {} dropped: {}", frame->header.seq, decoder.last_error());
            continue;
        }
        FrameInfo info{.seq = frame->header.seq};
        if (has_clock_offset_) {
            // t_capture_us is A's clock; subtracting the offset maps it to ours.
            info.t_capture_local_us = frame->header.t_capture_us - clock_offset_us_;
        }
        viewer_.present(spare, info);
        ++decoded_frames;
    }
    log::info("video: decode thread stopped after {} frames", decoded_frames);
}

}  // namespace kvm
