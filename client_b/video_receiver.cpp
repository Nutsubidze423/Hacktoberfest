// SPDX-License-Identifier: Apache-2.0

#include "video_receiver.h"

#include <utility>

#include "kvm/video_stats.h"

namespace kvm {

VideoReceiver::VideoReceiver(net::FrameChannel& channel, ViewerWindow& viewer) : channel_(channel), stream_(viewer) {
    read_thread_ = std::jthread([this, &viewer] {
        while (auto frame = channel_.recv_frame()) {
            video_stats().received("window", frame->payload.size());
            viewer.count_payload_bytes(frame->payload.size());
            stream_.push(std::move(*frame));
        }
        running_ = false;
        stream_.close();  // the decode threads finish what is left, then stop
    });
}

VideoReceiver::~VideoReceiver() {
    channel_.close();  // wakes recv_frame(); idempotent, so main.cpp may have closed it already
    stream_.close();
    // read_thread_ joins here, then the stream joins its own threads.
}

}  // namespace kvm
