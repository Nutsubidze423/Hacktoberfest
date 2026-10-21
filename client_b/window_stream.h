// SPDX-License-Identifier: Apache-2.0

// The viewer's frames (a window stream, not desk mode), decoded and shown in a ViewerWindow. Fed by
// whichever thread reads the video channel: VideoReceiver, or WindowLink's FrameRouter.
//   - "jpeg" frames go into a LatestSlot; a thread decodes the newest one and presents it. A slow
//     decoder drops old frames instead of queueing them.
//   - "h264" frames go into a short queue; a thread of their own decodes every one in order (a
//     picture refers to the one before it) on the viewer's GPU (h264_decoder.h), and shows the newest
//     picture. After a gap in `seq` (the queue overflowed, or the sender dropped one) or a frame that
//     fails the framing check (kvm/annexb.h), it waits for the next keyframe.
//   - Other codecs (e.g. kvm_host --test-video) count toward the HUD's MBPS but are not shown.
//
//   kvm::WindowStream stream(viewer);
//   while (auto frame = channel.recv_frame()) stream.push(std::move(*frame));   // one thread
//   stream.set_clock_offset_us(stats.clock_offset_us);                         // any thread
//
// Destroying it stops and joins its threads (frames still waiting are dropped).
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "kvm/queue.h"
#include "kvm/video.h"
#include "viewer_window.h"

namespace kvm {

class WindowStream {
public:
    explicit WindowStream(ViewerWindow& viewer);
    ~WindowStream();
    WindowStream(const WindowStream&) = delete;
    WindowStream& operator=(const WindowStream&) = delete;

    // A frame from the video channel (window layer). Call from one thread.
    void push(net::Frame frame);
    // No more frames: the threads finish what is waiting, then stop. Idempotent.
    void close();

    // The window owner's clock minus ours (net::LinkStats::clock_offset_us), any thread. Until it is
    // set the viewer shows latency as unknown.
    void set_clock_offset_us(std::int64_t offset) {
        clock_offset_us_ = offset;
        has_clock_offset_ = true;
    }

private:
    void jpeg_loop();
    void h264_loop();
    std::optional<std::int64_t> local_time(std::int64_t t_capture_us) const;

    static constexpr std::size_t kH264Queue = 30;  // a second at 30 fps; beyond that, wait for a keyframe

    ViewerWindow& viewer_;
    std::atomic<std::int64_t> clock_offset_us_{0};
    std::atomic<bool> has_clock_offset_{false};
    LatestSlot<net::Frame> latest_;
    BlockingQueue<net::Frame> h264_{kH264Queue};

    // Declared last: joined first on destruction, while everything above is still alive.
    std::jthread jpeg_thread_;
    std::jthread h264_thread_;
};

}  // namespace kvm
