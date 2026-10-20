// SPDX-License-Identifier: Apache-2.0

// Windows.Graphics.Capture (WGC) of one whole monitor (the VDD) or one window -> BGRA frames in
// CPU memory, delivered into a LatestSlot (only the newest frame waits; older ones are dropped).
//
//   kvm::LatestSlot<kvm::CapturedFrame> frames;
//   auto cap = kvm::ScreenCapture::for_monitor(vdd_rect->monitor);   // or for_window(hwnd)
//   cap->start(frames);
//   while (auto f = frames.take()) { ... encode and send ... }
//   cap->stop();                                                       // also done by the destructor
//
// Frames arrive on a WGC worker thread, and only when the content changes (at most at the display
// refresh rate). A custom callback must stay short; never call stop() from inside it.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "kvm/queue.h"

namespace kvm {

// BGRA, 8 bits per channel, top row first. Alpha is undefined (use TJPF_BGRX when encoding).
struct CapturedFrame {
    int width = 0;
    int height = 0;
    int stride = 0;  // bytes per row; always width * 4 (rows are tightly packed)
    std::vector<std::uint8_t> bgra;
    std::int64_t t_capture_us = 0;  // kvm::steady_now_us() when the frame arrived from WGC
};

class ScreenCapture {
public:
    using FrameCallback = std::function<void(CapturedFrame&&)>;
    using ClosedCallback = std::function<void()>;

    // True if this Windows version supports WGC (Windows 10 1903 or newer).
    static bool is_supported();

    // Throw std::runtime_error if the target can't be captured (closed window, no D3D11 device...).
    // capture_cursor: draw the mouse cursor into the frames.
    static std::unique_ptr<ScreenCapture> for_window(HWND hwnd, bool capture_cursor = false);
    static std::unique_ptr<ScreenCapture> for_monitor(HMONITOR monitor, bool capture_cursor = true);

    ~ScreenCapture();
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    // Starts putting frames into `out`, which must outlive the capture (or its stop()).
    // on_closed runs once if the window closes or the monitor goes away.
    // Throws std::runtime_error if the capture session can't be started.
    void start(LatestSlot<CapturedFrame>& out, ClosedCallback on_closed = {});

    // Same, with a custom callback instead of a slot.
    void start(FrameCallback on_frame, ClosedCallback on_closed = {});

    // Stops delivery. When it returns, no callback is running and none will run again.
    void stop();

private:
    struct Impl;
    explicit ScreenCapture(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace kvm
