// SPDX-License-Identifier: Apache-2.0

#include "window_share.h"

#include <format>
#include <stdexcept>

#include "kvm/log.h"

namespace kvm {

std::unique_ptr<WindowShare> WindowShare::start(HWND hwnd, net::FrameChannel& channel, Options options) {
    return start(
        hwnd,
        [&channel](const proto::FrameHeader& header, std::span<const std::uint8_t> payload) {
            return channel.send_frame(header, payload);
        },
        options);
}

std::unique_ptr<WindowShare> WindowShare::start(HWND hwnd, VideoSender::SendFn send, Options options) {
    if (!IsWindow(hwnd)) throw std::runtime_error("share: not a window (closed?)");

    // If any step below throws, `share` is destroyed and stop() undoes the steps done so far.
    std::unique_ptr<WindowShare> share(new WindowShare());
    share->hwnd_ = hwnd;
    share->vdd_ = VirtualDisplay::find();
    if (!share->vdd_) throw std::runtime_error("share: the Virtual Display Driver is not installed on this computer");

    if (!share->vdd_->attach(options.width, options.height, options.hz, options.side)) {
        throw std::runtime_error(
            std::format("share: can't attach the VDD at {}x{} @ {} Hz", options.width, options.height, options.hz));
    }
    const auto where = share->vdd_->rect();
    if (!where) throw std::runtime_error("share: the VDD is attached but has no monitor");
    share->rect_ = where->rect;

    switch (share->mover_.send(hwnd, share->rect_)) {
    case WindowMover::Result::Ok: break;
    case WindowMover::Result::Elevated:
        throw std::runtime_error("share: the window belongs to an admin process; run kvm_host as admin");
    case WindowMover::Result::NotAWindow: throw std::runtime_error("share: the window closed");
    default: throw std::runtime_error("share: can't move the window onto the VDD");
    }

    WindowShare* self = share.get();
    if (options.codec == "h264") {
        // Frames stay on the GPU from the capture to the encoder.
        std::string why = "no GPU with video support";
        auto gpu = GpuDevice::create();
        auto h264 = gpu ? H264Sender::create(gpu, share->width(), share->height(), send,
                                             {.max_fps = options.max_fps, .bitrate = options.h264_bitrate}, why)
                        : nullptr;
        if (h264) {
            share->h264_ = std::move(h264);
            share->capture_ = ScreenCapture::for_monitor(where->monitor, true, gpu);
            H264Sender* sender = share->h264_.get();
            share->capture_->start_gpu([sender](GpuFrame&& f) { sender->put(std::move(f)); },
                                       [self] { self->lost_ = true; });
        } else {
            log::warn("share: no H.264 ({}): sending JPEG", why);
        }
    }
    if (!share->h264_) {
        share->capture_ = ScreenCapture::for_monitor(where->monitor);
        share->capture_->start(share->frames_, [self] { self->lost_ = true; });
        share->sender_ = std::make_unique<VideoSender>(
            share->frames_, std::move(send),
            VideoSender::Options{.jpeg_quality = options.jpeg_quality, .max_fps = options.max_fps});
    }

    log::info("share: window is on the VDD ({}x{} at {}, {}), streaming {}", share->width(), share->height(),
              share->rect_.left, share->rect_.top, share->codec());
    return share;
}

WindowShare::~WindowShare() {
    stop();
}

void WindowShare::stop() {
    sender_.reset();          // 1. no more frames leave
    if (h264_) h264_->stop();  //    (the H.264 sender itself stays until the capture feeding it stops)
    if (capture_) {           // 2. capture stops before its monitor goes away
        capture_->stop();
        capture_.reset();
    }
    h264_.reset();
    if (mover_.sent_window()) mover_.restore(hwnd_);  // 3. window back while the VDD still exists
    if (vdd_) vdd_->detach();                         // 4. VDD off
}

}  // namespace kvm
