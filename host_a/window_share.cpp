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

    share->capture_ = ScreenCapture::for_monitor(where->monitor);
    WindowShare* self = share.get();
    share->capture_->start(share->frames_, [self] { self->lost_ = true; });
    share->sender_ = std::make_unique<VideoSender>(
        share->frames_, std::move(send),
        VideoSender::Options{.jpeg_quality = options.jpeg_quality, .max_fps = options.max_fps});

    log::info("share: window is on the VDD ({}x{} at {}, {}), streaming", share->width(), share->height(),
              share->rect_.left, share->rect_.top);
    return share;
}

WindowShare::~WindowShare() {
    stop();
}

void WindowShare::stop() {
    sender_.reset();  // 1. no more frames leave
    if (capture_) {   // 2. capture stops before its monitor goes away
        capture_->stop();
        capture_.reset();
    }
    if (mover_.sent_window()) mover_.restore(hwnd_);  // 3. window back while the VDD still exists
    if (vdd_) vdd_->detach();                         // 4. VDD off
}

}  // namespace kvm
