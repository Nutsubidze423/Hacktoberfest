// SPDX-License-Identifier: Apache-2.0

// The A-side video thread: newest captured frame -> JPEG -> video channel (port 50501).
//
//   kvm::LatestSlot<kvm::CapturedFrame> frames;              // capture->start(frames)
//   kvm::VideoSender sender(frames, *channel, {.jpeg_quality = cfg.jpeg_quality, .max_fps = cfg.max_fps});
//   ...
//   sender.stop();                                            // also done by the destructor
//
// Only the newest frame is ever sent: while one is encoded or sent, newer captures replace each
// other in the slot (LatestSlot::overwritten() counts the skipped ones). At most max_fps frames
// per second leave A. Every 5 s the thread logs fps, encode/send time, bitrate and skipped frames.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <stop_token>
#include <thread>

#include "capture.h"
#include "kvm/protocol.h"
#include "kvm/queue.h"
#include "kvm/video.h"

namespace kvm {

class VideoSender {
public:
    // Sends one encoded frame. false = the channel is closed; the sender then stops.
    using SendFn = std::function<bool(const proto::FrameHeader&, std::span<const std::uint8_t>)>;

    struct Options {
        int jpeg_quality = 75;  // config.json jpeg_quality
        int max_fps = 30;       // config.json max_fps; 0 = no limit
    };

    // Starts the sender thread, sending through the video channel (port 50501).
    // `frames` and `channel` must outlive the sender.
    VideoSender(LatestSlot<CapturedFrame>& frames, net::FrameChannel& channel, Options options);

    // Same with any send function (tests, other transports).
    VideoSender(LatestSlot<CapturedFrame>& frames, SendFn send, Options options);
    ~VideoSender();
    VideoSender(const VideoSender&) = delete;
    VideoSender& operator=(const VideoSender&) = delete;

    // Stops the thread (within ~100 ms). Doesn't close the slot: the capture may still write to it.
    void stop();

    // False after stop(), or once the channel has closed.
    bool running() const { return running_; }

    std::uint64_t frames_sent() const { return sent_; }

private:
    void run(std::stop_token stop);

    LatestSlot<CapturedFrame>& frames_;
    SendFn send_;
    Options options_;
    std::atomic<bool> running_{true};
    std::atomic<std::uint64_t> sent_{0};
    std::jthread thread_;  // last: starts after the members above exist
};

}  // namespace kvm
