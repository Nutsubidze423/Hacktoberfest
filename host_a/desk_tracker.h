// Desk mode, window owner (Temo): which windows are on the virtual display (VDD) right now.
//
//   kvm::DeskTracker tracker(region);    // region.set_vdd(...) first; follows later changes itself
//   ...                                  // region.rects() / region.hit(pt) are current within ~10 ms
//
// A thread listens to WinEvents (windows shown, hidden, moved, reordered, minimized, menus) and
// rescans after each burst, at most every 8 ms, and every 100 ms anyway. A window counts when it is
// visible, not minimized, not cloaked (other virtual desktops, suspended UWP apps), not fully
// transparent, and its frame (without the shadow) touches the VDD. The desktop, taskbars and this
// process's own windows never count: the peer shows its own desktop and taskbar there.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kvm/desk_region.h"

namespace kvm {

class DeskTracker {
public:
    struct Window {
        HWND hwnd = nullptr;
        display::Rect rect;  // its visible part on the VDD, screen coordinates
        std::string cls;
        std::string title;
    };

    explicit DeskTracker(desk::Region& region);
    ~DeskTracker();  // stops the thread; the region keeps the last rects

    DeskTracker(const DeskTracker&) = delete;
    DeskTracker& operator=(const DeskTracker&) = delete;

    // The windows of the last scan, topmost first (desk_test --list, logs).
    std::vector<Window> windows() const;

private:
    void run();
    void scan();

    desk::Region& region_;
    mutable std::mutex mutex_;
    std::vector<Window> windows_;
    std::atomic<DWORD> thread_id_{0};
    std::thread thread_;
};

}  // namespace kvm
