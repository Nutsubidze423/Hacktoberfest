// SPDX-License-Identifier: Apache-2.0

// Reads window-stream frames from the video channel and shows them in the viewer (window_stream.h
// decodes them: the newest JPEG, or every H.264 frame in order). For tools and tests: in the apps,
// WindowLink's FrameRouter reads the channel, since desk-mode frames share it.
//
//   auto video = kvm::net::connect_video(ip, cfg.video_port);    // A: VideoServer::accept()
//   kvm::VideoReceiver receiver(*video, viewer);                 // declare AFTER `video`
//   receiver.set_clock_offset_us(conn.stats().clock_offset_us);  // once rtt_us >= 0 (HUD's LAT)
//
// Destroying the receiver close()s the channel — the only way to wake recv_frame() — and joins its
// threads, so `video` can be destroyed right after. The receiver lives as long as the channel.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "kvm/video.h"
#include "viewer_window.h"
#include "window_stream.h"

namespace kvm {

class VideoReceiver {
public:
    VideoReceiver(net::FrameChannel& channel, ViewerWindow& viewer);
    ~VideoReceiver();

    VideoReceiver(const VideoReceiver&) = delete;
    VideoReceiver& operator=(const VideoReceiver&) = delete;

    // The window owner's clock minus ours (net::LinkStats::clock_offset_us), any thread. Until it is
    // set the viewer shows latency as unknown; 0 is exact when both ends are the same machine.
    void set_clock_offset_us(std::int64_t offset) { stream_.set_clock_offset_us(offset); }

    // False once the channel has closed (peer gone, or close() called).
    bool running() const { return running_; }

private:
    net::FrameChannel& channel_;
    std::atomic<bool> running_{true};
    WindowStream stream_;

    // Declared last: joined first on destruction, while the stream is still alive.
    std::jthread read_thread_;
};

}  // namespace kvm
