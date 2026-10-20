// SPDX-License-Identifier: Apache-2.0

// Continues a file drag on this PC with files that arrived from the other PC (T10, owner: Temo), the
// way Mouse Without Borders does it: the user grabbed files on the other PC, KVM brought the cursor
// here mid-drag, and the drag goes on here until the button goes up over any folder or the desktop,
// where Explorer copies the files.
//
//   kvm::files::FileDragSource drag;
//   drag.start({L"C:\\...\\Temp\\Sarke\\drag\\1\\photo.jpg"}, [](DWORD effect) { ... });
//
// Windows only lets a process run DoDragDrop if it owns the mouse button press, so start() puts a
// tiny, practically invisible window under the cursor and presses the left button on it (SendInput);
// the drag then ends when the button goes up, which KVM injects when the user lets go on the other PC.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "kvm/drag_image.h"

namespace kvm::files {

class FileDragSource {
public:
    // effect: DROPEFFECT_COPY / DROPEFFECT_MOVE if dropped somewhere, DROPEFFECT_NONE if cancelled.
    using OnDone = std::function<void(DWORD effect)>;

    FileDragSource();  // starts its own STA thread
    ~FileDragSource();
    FileDragSource(const FileDragSource&) = delete;
    FileDragSource& operator=(const FileDragSource&) = delete;

    // Starts dragging `files` from the cursor position. Returns at once; the drag runs on the
    // source's thread. False if a drag is already running or `files` is empty.
    // image: the picture to show under the cursor (the one the other PC's shell drew for these files,
    // so the drag looks the same on both screens); without it the shell draws a generic one.
    bool start(std::vector<std::filesystem::path> files, OnDone on_done = {},
               std::optional<DragImage> image = std::nullopt);

    bool dragging() const { return dragging_; }

    // The user let go (on the other PC) before this drag got running here, e.g. while the files were
    // still arriving: when it starts it drops at once, at `at` (where the cursor was when they let
    // go). Ignored once the drag runs: then the injected release drops it as usual.
    void note_release(POINT at);
    void forget_release();  // a new drag is on its way: an older early release doesn't count
    bool starting() const;  // start() was called and the drag doesn't run yet

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void on_start(HWND hwnd);        // window under the cursor + left button press
    void on_button_down(HWND hwnd);  // our press arrived: run DoDragDrop
    void finish(DWORD effect);

    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<bool> dragging_{false};
    mutable std::mutex mutex_;  // guards pending_ / on_done_ / running_ / early_release_
    std::vector<std::filesystem::path> pending_;
    OnDone on_done_;
    bool running_ = false;                // DoDragDrop is (about to be) running
    std::optional<POINT> early_release_;  // note_release() before the drag ran
    std::optional<DragImage> image_;      // start()'s image, for the next drag
    bool armed_ = false;  // on_start() pressed the button and waits for WM_LBUTTONDOWN (source thread)
    std::thread thread_;
};

}  // namespace kvm::files
