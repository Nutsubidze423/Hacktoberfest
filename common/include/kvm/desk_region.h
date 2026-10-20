// SPDX-License-Identifier: Apache-2.0

// Desk mode (owner: Temo): the other PC's screen is one more monitor of this PC. A Virtual Display
// Driver (VDD) monitor of the peer's size sits on the peer's side; windows dragged onto it keep
// running here and are drawn on the peer's screen at the same place (desk_share / desk_view).
//
// Region is what the parts share about that monitor:
//   - the VDD itself (device name, rect, whether desk mode is live) -> the KVM hook lets the cursor
//     onto it only while live, and leaves it out of the screen edges;
//   - which parts of it hold windows (rects, written by the window tracker ~100x a second) -> a click
//     there goes to the window here, anywhere else on the VDD it goes to the peer's own desktop.
//
//   kvm::desk::Region region;
//   region.set_vdd({.device = vdd.device(), .rect = r, .live = true});   // window_link
//   region.set_rects(rects);                                               // desk_tracker
//   if (region.hit(pt)) ...                                                // input_capture's hook
//
// Every method is thread-safe; vdd_rect() and hit() don't allocate, so the low-level hooks may call them.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>

#include "kvm/dpi.h"

namespace kvm::desk {

class Region {
public:
    static constexpr std::size_t kMaxRects = 64;

    struct Vdd {
        std::string device;    // display::Monitor::name, e.g. "\\.\DISPLAY5"; "" = no VDD
        display::Rect rect;    // where it is on the desktop; empty while detached
        bool live = false;     // desk mode is on: the peer shows it, the cursor may go there
    };

    Region() = default;
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;

    // The VDD changed (attached, moved, desk mode on/off). Calls the change callback.
    void set_vdd(Vdd vdd);
    Vdd vdd() const;

    // Allocation-free: false if there is no attached VDD. live = desk mode is on.
    bool vdd_rect(display::Rect& rect, bool& live) const;

    // True if `name` (display::Monitor::name) is the VDD.
    bool is_vdd(const std::string& name) const;

    // The visible windows on the VDD, screen coordinates, topmost first. At most kMaxRects are kept.
    // Bumps version() only if something changed.
    void set_rects(std::span<const display::Rect> rects);
    std::vector<display::Rect> rects() const;
    std::uint64_t version() const { return version_; }

    // True if p is on one of the windows (allocation-free, hook-safe).
    bool hit(display::Point p) const;

    // Runs after set_vdd(), on the caller's thread (e.g. posts a refresh to the hook thread).
    void set_on_change(std::function<void()> callback);

    // Where the user clicked last: on the VDD (the other PC's screen) or on this PC's own screens.
    // Windows that open soon after a click over there are moved over there (desk_tracker), like
    // apps opening on the monitor you work on. Hook-safe.
    void note_click(bool on_vdd);
    bool clicked_on_vdd_within(unsigned long long ms) const;

    // This PC's own mouse or keyboard was used (hook-safe). The other PC's mouse on our windows
    // doesn't pull our cursor away while our user is moving it.
    void note_local_input();
    bool local_input_within(unsigned long long ms) const;

private:
    mutable std::shared_mutex mutex_;
    std::string device_;
    display::Rect vdd_{};
    bool live_ = false;
    std::array<display::Rect, kMaxRects> rects_{};
    std::size_t count_ = 0;
    std::atomic<std::uint64_t> version_{0};

    std::mutex callback_mutex_;
    std::function<void()> on_change_;

    std::atomic<unsigned long long> click_ms_{0};  // GetTickCount64() of the last click
    std::atomic<bool> click_on_vdd_{false};
    std::atomic<unsigned long long> local_input_ms_{0};
};

}  // namespace kvm::desk
