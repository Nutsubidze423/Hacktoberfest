// SPDX-License-Identifier: Apache-2.0

// The viewer's frames (a window stream, not desk mode), decoded and shown in a ViewerWindow. Fed by
// whichever thread reads the video channel: VideoReceiver, or WindowLink's FrameRouter.
//   - "jpeg" frames go into a LatestSlot; a thread decodes the newest one and presents it. A slow
//     decoder drops old frames instead of queueing them.
//   - "h264" frames go into a short queue; a thread of their own decodes every one in order (a
//     picture refers to the one before it) on the viewer's GPU (h264_decoder.h), and shows the newest
//     picture. After a gap in `seq` (the queue overflowed, or the sender dropped one) or a frame that
//     fails the framing check (kvm/annexb.h) or the decoder, it waits for the next keyframe, and asks
//     the owner for one (Feedback::keyframe, at most every 500 ms). Once a second while it shows H.264,
//     it reports the latency and fps to the owner (Feedback::stats), which adapts its bitrate.
//   - Other codecs (e.g. kvm_host --test-video) count toward the HUD's MBPS but are not shown.
//
//   kvm::WindowStream stream(viewer, {.keyframe = [&] { send(proto::VideoKeyframe{}); },
//                                     .stats = [&](int ms, int fps) { send(proto::VideoStats{ms, fps}); }});
//   while (auto frame = channel.recv_frame()) stream.push(std::move(*frame));   // one thread
//   stream.set_clock_offset_us(stats.clock_offset_us);                         // any thread
//   stream.prepare("h264");                                                    // proto::VideoConfig
//
// Destroying it stops and joins its threads (frames still waiting are dropped).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>

#include "kvm/queue.h"
#include "kvm/video.h"
#include "viewer_window.h"

namespace kvm {

class WindowStream {
public:
    // What goes back to the window owner, called on the H.264 thread. Either may be empty.
    struct Feedback {
        std::function<void()> keyframe;                        // the decoder needs a keyframe
        std::function<void(int latency_ms, int fps)> stats;  // once a second while H.264 is shown
    };

    explicit WindowStream(ViewerWindow& viewer, Feedback feedback = {});
    ~WindowStream();
    WindowStream(const WindowStream&) = delete;
    WindowStream& operator=(const WindowStream&) = delete;

    // A frame from the video channel (window layer). Call from one thread.
    void push(net::Frame frame);
    // No more frames: the threads finish what is waiting, then stop. Idempotent.
    void close();

    // Frames of `codec` are coming (proto::VideoConfig): for "h264" the decoder starts now, not with
    // the first frame. Any thread.
    void prepare(const std::string& codec);

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

    static constexpr auto kKeyframeRequestGap = std::chrono::milliseconds(500);
    static constexpr auto kStatsEvery = std::chrono::seconds(1);

    static constexpr std::size_t kH264Queue = 30;  // a second at 30 fps; beyond that, wait for a keyframe

    ViewerWindow& viewer_;
    Feedback feedback_;
    std::atomic<bool> prepare_h264_{false};
    std::atomic<std::int64_t> clock_offset_us_{0};
    std::atomic<bool> has_clock_offset_{false};
    LatestSlot<net::Frame> latest_;
    BlockingQueue<net::Frame> h264_{kH264Queue};

    // Declared last: joined first on destruction, while everything above is still alive.
    std::jthread jpeg_thread_;
    std::jthread h264_thread_;
};

}  // namespace kvm
