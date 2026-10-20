// SPDX-License-Identifier: Apache-2.0

#include "kvm/desk_region.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <utility>

namespace kvm::desk {

void Region::set_vdd(Vdd vdd) {
    {
        std::unique_lock lock(mutex_);
        device_ = std::move(vdd.device);
        vdd_ = vdd.rect;
        live_ = vdd.live && vdd_.width() > 0 && vdd_.height() > 0;
        if (vdd_.width() <= 0 || vdd_.height() <= 0) count_ = 0;
    }
    ++version_;
    std::function<void()> callback;
    {
        std::lock_guard lock(callback_mutex_);
        callback = on_change_;
    }
    if (callback) callback();
}

Region::Vdd Region::vdd() const {
    std::shared_lock lock(mutex_);
    return {.device = device_, .rect = vdd_, .live = live_};
}

bool Region::vdd_rect(display::Rect& rect, bool& live) const {
    std::shared_lock lock(mutex_);
    if (vdd_.width() <= 0 || vdd_.height() <= 0) return false;
    rect = vdd_;
    live = live_;
    return true;
}

bool Region::is_vdd(const std::string& name) const {
    std::shared_lock lock(mutex_);
    return !device_.empty() && name == device_;
}

void Region::set_rects(std::span<const display::Rect> rects) {
    const std::size_t n = std::min(rects.size(), kMaxRects);
    {
        std::unique_lock lock(mutex_);
        if (n == count_ && std::equal(rects.begin(), rects.begin() + static_cast<std::ptrdiff_t>(n), rects_.begin())) {
            return;
        }
        std::copy_n(rects.begin(), n, rects_.begin());
        count_ = n;
    }
    ++version_;
}

std::vector<display::Rect> Region::rects() const {
    std::shared_lock lock(mutex_);
    return {rects_.begin(), rects_.begin() + static_cast<std::ptrdiff_t>(count_)};
}

bool Region::hit(display::Point p) const {
    std::shared_lock lock(mutex_);
    for (std::size_t i = 0; i < count_; ++i) {
        if (rects_[i].contains(p)) return true;
    }
    return false;
}

void Region::set_on_change(std::function<void()> callback) {
    std::lock_guard lock(callback_mutex_);
    on_change_ = std::move(callback);
}

void Region::note_click(bool on_vdd) {
    click_on_vdd_ = on_vdd;
    click_ms_ = GetTickCount64();
}

bool Region::clicked_on_vdd_within(unsigned long long ms) const {
    return click_on_vdd_ && GetTickCount64() - click_ms_ <= ms;
}

void Region::note_local_input() {
    local_input_ms_ = GetTickCount64();
}

bool Region::local_input_within(unsigned long long ms) const {
    return GetTickCount64() - local_input_ms_ <= ms;
}

}  // namespace kvm::desk
