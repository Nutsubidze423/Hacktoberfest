#include "frame_router.h"

#include <utility>

#include "../client_b/jpeg_decoder.h"
#include "kvm/log.h"

namespace kvm {

FrameRouter::FrameRouter(net::FrameChannel& channel, ViewerWindow& viewer, DeskView* desk)
    : channel_(channel), viewer_(viewer), desk_(desk) {
    read_thread_ = std::jthread([this] { read_loop(); });
    decode_thread_ = std::jthread([this] { decode_loop(); });
}

FrameRouter::~FrameRouter() {
    channel_.close();  // wakes recv_frame(); idempotent
    latest_.close();
    // jthread members join here, before the caller destroys the channel.
}

void FrameRouter::read_loop() {
    while (auto frame = channel_.recv_frame()) {
        if (frame->header.layer == "desk") {
            if (desk_) desk_->present(std::move(*frame));
            continue;
        }
        viewer_.count_payload_bytes(frame->payload.size());
        latest_.put(std::move(*frame));
    }
    latest_.close();
}

void FrameRouter::decode_loop() {
    JpegDecoder decoder;
    BgraImage spare;  // reused: present() hands back a spare buffer
    while (auto frame = latest_.take()) {
        if (frame->header.codec != "jpeg") continue;  // e.g. kvm_host --test-video: counted, not shown
        if (!decoder.decode(frame->payload, spare)) {
            log::warn("video: frame {} dropped: {}", frame->header.seq, decoder.last_error());
            continue;
        }
        FrameInfo info{.seq = frame->header.seq};
        if (has_clock_offset_) info.t_capture_local_us = frame->header.t_capture_us - clock_offset_us_;
        viewer_.present(spare, info);
    }
}

}  // namespace kvm
