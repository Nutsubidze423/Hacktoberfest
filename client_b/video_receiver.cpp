// SPDX-License-Identifier: Apache-2.0

#include "video_receiver.h"

#include <utility>

#include "jpeg_decoder.h"
#include "kvm/log.h"
#include "kvm/video_stats.h"

namespace kvm {

VideoReceiver::VideoReceiver(net::FrameChannel& channel, ViewerWindow& viewer) : channel_(channel), viewer_(viewer) {
    read_thread_ = std::jthread([this] { read_loop(); });
    decode_thread_ = std::jthread([this] { decode_loop(); });
}

VideoReceiver::~VideoReceiver() {
    channel_.close();  // wakes recv_frame(); idempotent, so main.cpp may have closed it already
    latest_.close();   // wakes the decode thread
    // jthread members join here, before the caller destroys the channel.
}

void VideoReceiver::read_loop() {
    while (auto frame = channel_.recv_frame()) {
        video_stats().received("window", frame->payload.size());
        viewer_.count_payload_bytes(frame->payload.size());
        latest_.put(std::move(*frame));
    }
    running_ = false;
    latest_.close();  // the decode thread finishes what is left, then stops
}

void VideoReceiver::decode_loop() {
    JpegDecoder decoder;
    BgraImage spare;  // reused: present() hands back a spare buffer
    bool codec_warned = false;
    std::uint64_t decoded_frames = 0;

    while (auto frame = latest_.take()) {
        if (frame->header.codec != "jpeg") {
            if (!codec_warned) {
                log::info("video: codec '{}' is not shown (counted for MBPS only)", frame->header.codec);
                codec_warned = true;
            }
            continue;
        }
        const std::int64_t started = steady_now_us();
        if (!decoder.decode(frame->payload, spare)) {
            log::warn("video: frame {} dropped: {}", frame->header.seq, decoder.last_error());
            continue;
        }
        const std::int64_t decoded = steady_now_us();
        FrameInfo info{.seq = frame->header.seq,
                       .bytes = frame->payload.size(),
                       .decode_us = decoded - started,
                       .decoded_at_us = decoded};
        if (has_clock_offset_) {
            // t_capture_us is the owner's clock; subtracting the offset maps it to ours
            // (the same as Connection::peer_to_local_us).
            info.t_capture_local_us = frame->header.t_capture_us - clock_offset_us_;
        }
        viewer_.present(spare, info);
        ++decoded_frames;
    }
    log::info("video: receiver stopped after {} decoded frames", decoded_frames);
}

}  // namespace kvm
