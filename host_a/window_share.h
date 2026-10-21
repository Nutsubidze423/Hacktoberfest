// SPDX-License-Identifier: Apache-2.0

// One window "teleported" to the other computer: it leaves this screen for the VDD, the VDD is
// captured, encoded and streamed; the other side shows it in a viewer and sends input back.
// Destroying the share brings everything back in the right order: sender, capture, window, VDD.
// Works on whichever computer owns the window (A today, B too once it links this code).
//
//   auto share = kvm::WindowShare::start(hwnd, *video_channel, {.width = hello.screen_w, ...});
//   conn->send(proto::StreamStart{.hwnd = ..., .width = share->width(), .height = share->height()});
//   ... remote input: x_norm/y_norm -> share->monitor_rect() -> SendInput ...
//   share.reset();                                    // window back where it was, VDD off
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include "capture.h"
#include "h264_sender.h"
#include "kvm/dpi.h"
#include "kvm/queue.h"
#include "kvm/video.h"
#include "video_sender.h"
#include "virtual_display.h"
#include "window_mover.h"

namespace kvm {

class WindowShare {
public:
    struct Options {
        int width = 1920;  // the viewer's screen (hello.screen_w / screen_h); the VDD gets this mode
        int height = 1080;
        int hz = 60;
        display::Side side = display::Side::Right;  // where the VDD goes, config.json client_side
        int jpeg_quality = 75;
        int max_fps = 30;
        std::string codec = "jpeg";  // "h264": encoded on the GPU (h264_sender.h); JPEG if it can't be
        int h264_bitrate = 8'000'000;
    };

    // Moves `hwnd` onto the VDD and starts streaming it. Throws std::runtime_error with the reason
    // (no VDD driver, elevated window -> "run kvm_host as admin", capture failure...); every step
    // done so far is undone first, so the window and the desktop are as before.
    static std::unique_ptr<WindowShare> start(HWND hwnd, net::FrameChannel& channel, Options options);
    static std::unique_ptr<WindowShare> start(HWND hwnd, VideoSender::SendFn send, Options options);

    ~WindowShare();
    WindowShare(const WindowShare&) = delete;
    WindowShare& operator=(const WindowShare&) = delete;

    HWND window() const { return hwnd_; }

    // The VDD in physical pixels: remote x_norm / y_norm map onto this rect.
    const RECT& monitor_rect() const { return rect_; }
    int width() const { return rect_.right - rect_.left; }
    int height() const { return rect_.bottom - rect_.top; }

    // False once the shared app closed its window or the VDD went away: time to end the share.
    bool alive() const { return !lost_ && IsWindow(hwnd_); }

    std::uint64_t frames_sent() const { return h264_ ? h264_->frames_sent() : sender_ ? sender_->frames_sent() : 0; }
    // "h264" or "jpeg": what the frames are sent as.
    const char* codec() const { return h264_ ? "h264" : "jpeg"; }
    // The viewer's decoder needs a keyframe (proto::VideoKeyframe). H.264 only; rate-limited.
    void request_keyframe() {
        if (h264_) h264_->request_keyframe();
    }

private:
    WindowShare() = default;
    void stop();

    HWND hwnd_ = nullptr;
    RECT rect_{};
    std::optional<VirtualDisplay> vdd_;
    WindowMover mover_;
    LatestSlot<CapturedFrame> frames_;
    std::unique_ptr<ScreenCapture> capture_;
    std::unique_ptr<VideoSender> sender_;
    std::unique_ptr<H264Sender> h264_;
    std::atomic<bool> lost_ = false;
};

}  // namespace kvm
