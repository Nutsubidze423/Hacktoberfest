// SPDX-License-Identifier: Apache-2.0

// Desk mode, window owner (Temo): which windows are on the virtual display (VDD) right now.
//
//   kvm::DeskTracker tracker(region, [&](const auto& windows) { ... });  // region.set_vdd(...) first
//   ...                                  // region.rects() / region.hit(pt) are current within ~10 ms
//
// A thread listens to WinEvents (windows shown, hidden, moved, reordered, minimized, menus) and
// rescans after each burst, at most every 8 ms, and every 100 ms anyway. A window counts when it is
// visible, not minimized, not cloaked (other virtual desktops, suspended UWP apps), not fully
// transparent, and its frame (without the shadow) touches the VDD. The desktop, taskbars, drag
// images and this process's own windows never count: the peer shows its own desktop and taskbar.
//
// New app windows that open while the user works on the other PC's screen (the last click was on
// the VDD) are moved onto the VDD, the way Windows opens apps on the monitor you are using: opening
// a file from a folder window on the other screen shows the app over there.
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
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "kvm/desk_region.h"

namespace kvm {

class DeskTracker {
public:
    struct Window {
        HWND hwnd = nullptr;
        display::Rect frame;  // the whole frame (no shadow), screen coordinates
        display::Rect rect;   // the part of it on the VDD that takes clicks (incl. resize borders), screen coordinates
        std::string cls;
        std::string title;
    };

    // Called on the tracker's thread with the windows (topmost first) whenever one appears,
    // disappears, moves or changes order.
    using Changed = std::function<void(const std::vector<Window>&)>;

    explicit DeskTracker(desk::Region& region, Changed on_change = {});
    ~DeskTracker();  // stops the thread; the region keeps the last rects

    DeskTracker(const DeskTracker&) = delete;
    DeskTracker& operator=(const DeskTracker&) = delete;

    // The windows of the last scan, topmost first (desk_test --list, logs).
    std::vector<Window> windows() const;

private:
    void run();
    void scan();
    void place_new_windows();  // tracker thread: new app windows opened from the other screen

    desk::Region& region_;
    Changed on_change_;
    mutable std::mutex mutex_;
    std::vector<Window> windows_;
    std::unordered_set<HWND> known_;  // tracker thread: top-level windows seen so far
    std::atomic<DWORD> thread_id_{0};
    std::thread thread_;
};

}  // namespace kvm
