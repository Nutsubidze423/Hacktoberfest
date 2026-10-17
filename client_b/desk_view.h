// Desk mode, viewer side (Temo): draws the other PC's windows that sit on its virtual display at the
// same place on this PC's primary monitor, on top of everything, and nothing else. Between and around
// them this PC's own desktop and windows stay visible and clickable, so the other PC's screen looks
// like one more monitor that windows can be dragged onto.
//
//   kvm::DeskView desk;                                   // starts its own UI and decode threads
//   desk.set_input_sink([&](const kvm::proto::Message& m) { return send(m); });
//   desk.present(std::move(frame));                       // layer "desk" frames from the video channel
//   desk.clear();                                         // link lost: hide everything
//
// The overlay is one topmost, never-activated layered window over the primary monitor with
// per-pixel alpha: 255 on the windows, 0 elsewhere. Windows passes clicks on alpha-0 pixels to
// whatever is below, so only the drawn windows take input.
//
// Input: this PC's own mouse and keyboard on the drawn windows go to their owner (remote_mouse_move,
// mouse_button, mouse_wheel, key; positions normalized over the owner's virtual display, not clamped
// while a button is held so a window can be dragged back across). Injected input (the KVM moving
// this cursor for the other PC) is ignored: the owner already has it.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include "jpeg_decoder.h"
#include "kvm/protocol.h"
#include "kvm/queue.h"
#include "kvm/video.h"
#include "viewer_input.h"

namespace kvm {

class DeskView {
public:
    using Sink = std::function<bool(const proto::Message&)>;

    DeskView();
    ~DeskView();

    DeskView(const DeskView&) = delete;
    DeskView& operator=(const DeskView&) = delete;

    // A layer "desk" frame (any thread). Only the newest one waiting is decoded.
    void present(net::Frame&& frame);

    // Hides the overlay and releases held input (any thread), e.g. when the link is lost.
    void clear();

    // Where this PC's input on the drawn windows goes (the window owner). nullptr = nowhere.
    void set_input_sink(Sink sink);

    // The owner's cursor shape over its windows (desk_cursor), any thread.
    void set_cursor_shape(std::string_view shape);

    // Frames drawn so far (tests, logs).
    std::uint64_t frames_shown() const { return shown_; }
    HWND hwnd() const { return hwnd_; }

private:
    struct Pending {
        proto::FrameHeader header;
        BgraImage image;
    };

    void ui_main(std::promise<void>* ready);
    void decode_main();
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    bool make_surface();     // the DIB the size of the primary monitor
    void free_surface();
    void show_pending();     // UI thread: composes the newest frame and updates the window
    void hide();             // UI thread
    bool on_input(UINT msg, WPARAM wp, LPARAM lp);  // UI thread; true = handled
    NormPoint norm(LPARAM lp) const;

    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<DWORD> ui_thread_id_{0};
    LatestSlot<net::Frame> incoming_;

    std::mutex pending_mutex_;
    std::optional<Pending> pending_;

    std::mutex sink_mutex_;
    Sink sink_;
    std::atomic<HCURSOR> cursor_{nullptr};
    std::atomic<std::uint64_t> shown_{0};

    // UI thread only.
    HDC mem_dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_bitmap_ = nullptr;
    std::uint8_t* bits_ = nullptr;
    RECT screen_{};                     // the primary monitor
    std::vector<RECT> drawn_;           // where the surface is opaque now
    bool first_update_ = true;
    bool visible_ = false;
    std::vector<proto::DeskRect> last_rects_;
    int desk_w_ = 0;
    int desk_h_ = 0;
    ViewerInput input_;
    int buttons_held_ = 0;

    std::thread ui_thread_;
    std::jthread decode_thread_;
};

}  // namespace kvm
