// SPDX-License-Identifier: Apache-2.0

// The video threads for H.264 (roadmap P2.4c): the newest captured GPU frame -> NV12 -> the hardware
// encoder -> the video channel. The H.264 counterpart of VideoSender (JPEG), with the same pacing:
// at most max_fps leave, and the newest capture wins. A still window is sent again every 500 ms, as a
// new (almost empty) picture: the same access unit twice would break the viewer's decoder.
//
//   auto gpu = kvm::GpuDevice::create();
//   auto sender = kvm::H264Sender::create(gpu, w, h, send, {.max_fps = 30, .bitrate = 8'000'000}, why);
//   auto capture = kvm::ScreenCapture::for_monitor(monitor, true, gpu);
//   capture->start_gpu([&](kvm::GpuFrame&& f) { sender->put(std::move(f)); });
//
// A frame of another size starts a new encoder (its stream begins with a keyframe). Until codec
// negotiation and keyframe requests (P2.5, P2.6), a keyframe goes out every keyframe_interval_s, so
// a viewer that connects mid-stream shows a picture within that time.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

#include "capture.h"
#include "h264_encoder.h"
#include "kvm/gpu_device.h"
#include "kvm/queue.h"
#include "nv12_converter.h"
#include "video_sender.h"

namespace kvm {

class H264Sender {
public:
    struct Options {
        int max_fps = 30;
        int bitrate = 8'000'000;
        int keyframe_interval_s = 2;
    };

    // Starts the threads, with an encoder for width x height frames on `gpu`. nullptr with `why` set
    // if there is no hardware H.264 encoder.
    static std::unique_ptr<H264Sender> create(std::shared_ptr<GpuDevice> gpu, int width, int height,
                                              VideoSender::SendFn send, Options options, std::string& why);
    ~H264Sender();
    H264Sender(const H264Sender&) = delete;
    H264Sender& operator=(const H264Sender&) = delete;

    // A captured frame (any thread; the capture's).
    void put(GpuFrame&& frame) { frames_.put(std::move(frame)); }
    // Stops both threads. Also done by the destructor.
    void stop();

    std::uint64_t frames_sent() const { return sent_; }

private:
    H264Sender() = default;
    void feed_loop(std::stop_token stop);  // frames -> encoder
    void send_loop(std::stop_token stop);  // encoder packets -> channel
    std::shared_ptr<H264Encoder> encoder_for(int width, int height);

    std::shared_ptr<GpuDevice> gpu_;
    VideoSender::SendFn send_;
    Options options_;
    std::unique_ptr<Nv12Converter> nv12_;
    LatestSlot<GpuFrame> frames_;

    std::mutex mutex_;  // encoder_ (replaced on a size change), read by the send thread
    std::condition_variable encoder_changed_;
    std::shared_ptr<H264Encoder> encoder_;
    int refused_w_ = 0;  // the size an encoder couldn't be made for
    int refused_h_ = 0;
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<bool> channel_closed_{false};

    std::jthread feed_thread_;  // last: they start after the members above exist
    std::jthread send_thread_;
};

}  // namespace kvm
