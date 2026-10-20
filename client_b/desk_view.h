// SPDX-License-Identifier: Apache-2.0

// Desk mode, viewer side (Temo): draws the other PC's windows that sit on its virtual display at the
// same place on this PC's primary monitor, on top of everything, and nothing else. Between and around
// them this PC's own desktop and windows stay visible and clickable, so the other PC's screen looks
// like one more monitor that windows can be dragged onto.
//
//   kvm::DeskView desk;                                   // starts its own UI and decode threads
//   desk.set_input_sink([&](const kvm::proto::Message& m) { return send(m); });
//   desk.set_layout(layout);                              // desk_layout: which windows, where, in what order
//   desk.present(std::move(frame));                       // layer "desk" frames: one window's picture
//   desk.clear();                                         // link lost: hide everything
//
// Every window's newest picture is kept and drawn at its place from the newest layout, topmost last,
// with its exact transparency (rounded corners). Moving a window only changes the layout, so it moves
// as smoothly as the layout messages arrive. The overlay is one topmost, never-activated layered
// window over the primary monitor with per-pixel alpha; Windows passes clicks on its transparent
// pixels to whatever is below, so only the drawn windows take input.
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
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include "kvm/protocol.h"
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

    // A layer "desk" frame: one window's picture (any thread). Per window only the newest is decoded.
    void present(net::Frame&& frame);

    // The owner's windows and their places (any thread).
    void set_layout(proto::DeskLayout layout);

    // Hides the overlay, forgets every window and releases held input (any thread), e.g. link lost.
    void clear();

    // Where this PC's input on the drawn windows goes (the window owner). nullptr = nowhere.
    void set_input_sink(Sink sink);

    // The owner's cursor shape over its windows (desk_cursor), any thread.
    void set_cursor_shape(std::string_view shape);

    // Updates drawn so far (tests, logs).
    std::uint64_t frames_shown() const { return shown_; }
    HWND hwnd() const { return hwnd_; }

    // Tests: what the overlay holds now (premultiplied BGRA of the primary monitor), as a 32-bit BMP.
    bool save_surface(const std::wstring& path);

private:
    // One window's picture: premultiplied BGRA, top-down, no padding.
    struct Picture {
        int width = 0;
        int height = 0;
        int x = 0;  // where it sits inside the window's frame
        int y = 0;
        std::vector<std::uint8_t> pixels;
        std::vector<std::uint8_t> opaque_rows;  // 1 = the whole row has alpha 255
    };
    struct Placed {
        std::uint64_t id = 0;
        RECT rect{};  // on the surface, clipped
        RECT hit{};   // rect plus an invisible rim (alpha 1) that takes clicks for the owner's resize borders
        int src_x = 0;
        int src_y = 0;
    };

    // Which machine's windows are in front on this screen (desk mode is two z-orders merged into one):
    // the owner's (the drawn overlay on top) or this PC's own (a local window the user raised).
    enum class Front { Owner, Local };

    void ui_main(std::promise<void>* ready);
    void decode_main(std::stop_token stop);
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static void CALLBACK on_foreground_event(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);

    bool make_surface();  // the DIB the size of the primary monitor
    void free_surface();
    void update();        // UI thread: applies the newest layout and pictures and redraws what changed
    void compose(const std::vector<Placed>& placed, const RECT& dirty);
    void hide();          // UI thread
    void apply_z();       // UI thread: puts the overlay above or below this PC's own windows per front_
    void on_local_foreground(HWND fg);  // UI thread: this PC raised one of its own windows
    bool on_input(UINT msg, WPARAM wp, LPARAM lp);  // UI thread; true = handled
    NormPoint norm(LPARAM lp) const;

    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<DWORD> ui_thread_id_{0};

    // decode thread input
    std::mutex incoming_mutex_;
    std::condition_variable incoming_cv_;
    std::map<std::uint64_t, net::Frame> incoming_;  // newest undecoded frame per window

    // decode/session threads -> UI thread
    std::mutex pending_mutex_;
    std::optional<proto::DeskLayout> pending_layout_;
    std::map<std::uint64_t, Picture> pending_pictures_;
    bool pending_clear_ = false;

    std::mutex sink_mutex_;
    Sink sink_;
    std::atomic<HCURSOR> cursor_{nullptr};
    std::atomic<std::uint64_t> shown_{0};

    // UI thread only.
    HDC mem_dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_bitmap_ = nullptr;
    std::uint8_t* bits_ = nullptr;
    RECT screen_{};  // the primary monitor
    proto::DeskLayout layout_;
    std::map<std::uint64_t, Picture> pictures_;
    std::vector<Placed> placed_;  // what is drawn now, bottom first
    bool first_update_ = true;
    bool visible_ = false;
    Front front_ = Front::Owner;  // a window just dragged over starts in front
    HWND local_foreground_ = nullptr;  // this PC's window to stay below while front_ == Local
    HWINEVENTHOOK foreground_hook_ = nullptr;
    ViewerInput input_;
    int buttons_held_ = 0;

    std::thread ui_thread_;
    std::jthread decode_thread_;
};

}  // namespace kvm
