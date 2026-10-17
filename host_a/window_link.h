// Window teleport glue for kvm_host and kvm_client (M1b; owner: Temo): one per process, the same on
// both PCs. A window of this PC goes to the other PC's viewer and is controlled from there; a window
// of the other PC shows up here in Sandro's viewer.
//
//   kvm::WindowLink windows({.jpeg_quality = q, .max_fps = fps, .peer_side = side});
//   windows.set_video(channel);                  // each video connection (shared, both directions)
//   per session:
//     windows.attach(send, peer_w, peer_h);      // send = control messages to the peer
//     if (!kvm.handle(m) && !windows.handle(m)) { ... }
//     windows.detach();                          // window back here, viewer shows "lost"
//   hotkey Ctrl+Alt+Shift+W -> windows.toggle();
//
// toggle(): the foreground window goes to the other PC (WindowShare: VDD, capture, JPEG, stream_start)
// or, if one is there already, comes back. With the viewer in front, the other PC's window goes home.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "kvm/dpi.h"
#include "kvm/protocol.h"
#include "kvm/video.h"

namespace kvm {

class ViewerWindow;
class VideoReceiver;
class WindowShare;
class RemoteInput;

class WindowLink {
public:
    using Sink = std::function<bool(const proto::Message&)>;

    struct Options {
        int jpeg_quality = 75;
        int max_fps = 30;
        display::Side peer_side = display::Side::Right;  // where the VDD goes for an outgoing window
    };

    explicit WindowLink(Options options);
    ~WindowLink();  // window back here, viewer closed

    WindowLink(const WindowLink&) = delete;
    WindowLink& operator=(const WindowLink&) = delete;

    // The current video connection (port 50501). Frames of this PC's window go out on it and the
    // other PC's frames are read from it. nullptr = none.
    void set_video(std::shared_ptr<net::FrameChannel> channel);

    // A control session starts: `send` reaches the peer, peer_w x peer_h is its screen (the VDD size).
    void attach(Sink send, int peer_w, int peer_h);
    void set_peer_screen(int peer_w, int peer_h);
    // The session ended: this PC's window comes back, the viewer shows the link as lost.
    void detach();

    // stream_start / stream_stop / remote_* / key / mouse_* from the peer. False = not for windows.
    bool handle(const proto::Message& msg);

    // Ctrl+Alt+Shift+W: send the foreground window over, or bring it back (see above).
    void toggle();

    void set_rtt_ms(int rtt_ms, std::int64_t clock_offset_us);

private:
    void ui_loop();
    void stop_share(bool tell_peer);  // mutex_ held
    bool send(const proto::Message& msg);

    Options options_;
    // mutex_: share_, input_, viewer_, peer size. video_mutex_: video_, receiver_ (never held while
    // waiting on mutex_, so stopping a share can join its sender thread). send_mutex_: send_.
    std::mutex mutex_;
    std::mutex video_mutex_;
    std::mutex send_mutex_;
    Sink send_;
    int peer_w_ = 1920;
    int peer_h_ = 1080;
    std::shared_ptr<net::FrameChannel> video_;
    std::unique_ptr<VideoReceiver> receiver_;
    std::unique_ptr<WindowShare> share_;
    std::unique_ptr<RemoteInput> input_;
    std::unique_ptr<ViewerWindow> viewer_;
    std::atomic<HWND> viewer_hwnd_{nullptr};
    std::atomic<DWORD> ui_thread_id_{0};
    std::thread ui_thread_;
    std::jthread watchdog_;  // ends a share whose window was closed
};

}  // namespace kvm
