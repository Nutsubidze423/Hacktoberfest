// SPDX-License-Identifier: Apache-2.0

// Reads window-stream frames from the video channel, decodes the newest one and shows it in the
// viewer.
//
// main.cpp owns the channel: one FrameChannel carries frames both ways (A's window to B and B's
// window to A), and connecting / reconnecting is main.cpp's job. The receiver only reads from it:
//
//   auto video = kvm::net::connect_video(ip, cfg.video_port);    // A: VideoServer::accept()
//   kvm::VideoReceiver receiver(*video, viewer);                 // declare AFTER `video`
//   receiver.set_clock_offset_us(conn.stats().clock_offset_us);  // once rtt_us >= 0 (HUD's LAT)
//
// Destroying the receiver close()s the channel — the only way to wake recv_frame() — and joins its
// threads, so `video` can be destroyed right after. The receiver lives as long as the channel.
//
// Threads: the read thread is the channel's only recv_frame() caller and puts frames into a
// LatestSlot; the decode thread decodes the newest one and presents it. A slow decoder drops old
// frames instead of queueing them. Frames whose codec is not "jpeg" (e.g. kvm_host --test-video)
// count toward the HUD's MBPS but are not shown.
#pragma once

#include <atomic>
#include <cstdint>
#include <stop_token>
#include <thread>

#include "kvm/queue.h"
#include "kvm/video.h"
#include "viewer_window.h"

namespace kvm {

class VideoReceiver {
public:
    VideoReceiver(net::FrameChannel& channel, ViewerWindow& viewer);
    ~VideoReceiver();

    VideoReceiver(const VideoReceiver&) = delete;
    VideoReceiver& operator=(const VideoReceiver&) = delete;

    // The window owner's clock minus ours (net::LinkStats::clock_offset_us), any thread. Until it is
    // set the viewer shows latency as unknown; 0 is exact when both ends are the same machine.
    void set_clock_offset_us(std::int64_t offset) {
        clock_offset_us_ = offset;
        has_clock_offset_ = true;
    }

    // False once the channel has closed (peer gone, or close() called).
    bool running() const { return running_; }

private:
    void read_loop();
    void decode_loop();

    net::FrameChannel& channel_;
    ViewerWindow& viewer_;
    std::atomic<std::int64_t> clock_offset_us_{0};
    std::atomic<bool> has_clock_offset_{false};
    std::atomic<bool> running_{true};
    LatestSlot<net::Frame> latest_;

    // Declared last: joined first on destruction, while everything above is still alive.
    std::jthread read_thread_;
    std::jthread decode_thread_;
};

}  // namespace kvm
