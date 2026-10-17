// Desk mode, window owner (Temo): streams the windows on the virtual display (VDD) to the other PC,
// which draws them at the same place on its screen (client_b/desk_view).
//
//   kvm::desk::Region region;   region.set_vdd({.device = ..., .rect = vdd_rect->rect, .live = true});
//   auto share = std::make_unique<kvm::DeskShare>(*vdd_rect, region, send_frame, {.jpeg_quality = 75, .max_fps = 30});
//   ...
//   share.reset();              // stops capture, tracker and sender
//
// Pipeline: WGC captures the whole VDD (without the cursor: the peer shows its real one) into a
// LatestSlot; a DeskTracker keeps the Region's window rects current; the sender thread crops the
// newest frame to the rects' bounding box, JPEG-encodes it and sends it with layer "desk" and the
// rects. It also sends when only the rects changed (a window closed, a menu opened), and once a
// codec "none" frame when no window is left, so the peer hides the overlay. At most max_fps frames
// per second; the newest frame always wins. Every 5 s it logs fps, size and bitrate.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <thread>

#include "capture.h"
#include "desk_tracker.h"
#include "kvm/desk_region.h"
#include "kvm/queue.h"
#include "video_sender.h"
#include "virtual_display.h"

namespace kvm {

class DeskShare {
public:
    using SendFn = VideoSender::SendFn;

    struct Options {
        int jpeg_quality = 75;
        int max_fps = 30;
    };

    // `vdd` must be attached; `region` must already hold it and outlive the share.
    // Throws std::runtime_error if the VDD can't be captured.
    DeskShare(const VirtualDisplay::MonitorRect& vdd, desk::Region& region, SendFn send, Options options);
    ~DeskShare();

    DeskShare(const DeskShare&) = delete;
    DeskShare& operator=(const DeskShare&) = delete;

    // False once the capture ended (the VDD went away).
    bool alive() const { return !lost_; }
    std::uint64_t frames_sent() const { return sent_; }
    const DeskTracker& tracker() const { return *tracker_; }

private:
    void run(std::stop_token stop);

    desk::Region& region_;
    SendFn send_;
    Options options_;
    std::atomic<bool> lost_{false};
    std::atomic<std::uint64_t> sent_{0};
    LatestSlot<CapturedFrame> frames_;
    std::unique_ptr<DeskTracker> tracker_;
    std::unique_ptr<ScreenCapture> capture_;
    std::jthread thread_;  // last: starts after everything above exists, stops first
};

}  // namespace kvm
