// SPDX-License-Identifier: Apache-2.0

#include "desk_input.h"

#include <windows.h>

#include <cmath>
#include <variant>

#include "kvm/win32_input.h"

namespace kvm {

namespace {

// While this PC's own user moves the mouse, the other PC's mere hovering doesn't pull the cursor
// away from them (clicks and drags still go through: someone over there is using our window).
constexpr unsigned long long kLocalWinsMs = 700;

}  // namespace

bool DeskInput::move_to(double x_norm, double y_norm) {
    display::Rect vdd;
    bool live = false;
    if (!region_.vdd_rect(vdd, live) || !live) return false;
    const int x = vdd.left + static_cast<int>(std::lround(x_norm * vdd.width()));
    const int y = vdd.top + static_cast<int>(std::lround(y_norm * vdd.height()));
    input::Point cur;
    if (input::get_cursor_pos(cur) && cur.x == x && cur.y == y) return true;
    return input::move_cursor_abs(x, y);  // clamped to the desktop: past the edge = our real monitors
}

bool DeskInput::handle(const proto::Message& msg) {
    return std::visit(
        proto::Overloaded{
            [&](const proto::RemoteMouseMove& m) {
                if (buttons_.empty() && region_.local_input_within(kLocalWinsMs)) return true;
                move_to(m.x_norm, m.y_norm);
                return true;
            },
            [&](const proto::MouseButtonEvent& m) {
                // Down: the window under the point gets the click (and activates) natively.
                if (m.down) {
                    if (!move_to(m.x_norm, m.y_norm)) return true;
                    buttons_.insert(m.button);
                    region_.note_click(/*on_vdd=*/true);  // apps opened from here show up over there
                } else if (!buttons_.erase(m.button)) {
                    return true;  // never an up for a down we didn't replay
                }
                input::send_mouse_button(m.button, m.down);
                return true;
            },
            [&](const proto::MouseWheel& m) {
                input::send_wheel(m.delta, m.horizontal);
                return true;
            },
            [&](const proto::Key& k) {
                // The Windows key would open our Start menu on our own screen, where nobody sees it.
                if (k.vk == VK_LWIN || k.vk == VK_RWIN) return true;
                const auto id = std::tuple{k.vk, k.scan, k.extended};
                if (k.down) {
                    keys_.insert(id);
                } else if (!keys_.erase(id)) {
                    return true;
                }
                input::send_key(k.vk, k.scan, k.down, k.extended);
                return true;
            },
            [](const auto&) { return false; },
        },
        msg);
}

void DeskInput::release_all() {
    for (const auto b : buttons_) input::send_mouse_button(b, false);
    for (const auto& [vk, scan, extended] : keys_) input::send_key(vk, scan, false, extended);
    buttons_.clear();
    keys_.clear();
}

}  // namespace kvm
