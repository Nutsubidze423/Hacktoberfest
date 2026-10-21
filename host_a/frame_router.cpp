// SPDX-License-Identifier: Apache-2.0

#include "frame_router.h"

#include <utility>

#include "kvm/video_stats.h"

namespace kvm {

FrameRouter::FrameRouter(net::FrameChannel& channel, ViewerWindow& viewer, DeskView* desk,
                         WindowStream::Feedback feedback)
    : channel_(channel), viewer_(viewer), desk_(desk), window_(viewer, std::move(feedback)) {
    read_thread_ = std::jthread([this] { read_loop(); });
}

FrameRouter::~FrameRouter() {
    channel_.close();  // wakes recv_frame(); idempotent
    window_.close();
    // read_thread_ joins here, then the window stream joins its threads, before the caller destroys
    // the channel.
}

void FrameRouter::read_loop() {
    while (auto frame = channel_.recv_frame()) {
        if (frame->header.layer == "desk") {
            video_stats().received("desk", frame->payload.size());
            std::optional<std::int64_t> t_local;
            if (has_clock_offset_) t_local = frame->header.t_capture_us - clock_offset_us_;
            if (desk_) desk_->present(std::move(*frame), t_local);
            continue;
        }
        video_stats().received("window", frame->payload.size());
        viewer_.count_payload_bytes(frame->payload.size());
        window_.push(std::move(*frame));
    }
    window_.close();
}

}  // namespace kvm
