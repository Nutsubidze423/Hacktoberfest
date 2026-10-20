// SPDX-License-Identifier: Apache-2.0

#pragma once
// Thin wrappers over SendInput for mouse and keyboard emulation.
//
// Used by client_b/input_inject (KVM on B) and host_a/remote_input (viewer
// input replayed into A's window). All coordinates are physical pixels on the
// virtual desktop, so the process must be per-monitor DPI aware (see dpi.h)
// before calling anything here.
//
// Every event is tagged with kInjectedMarker in dwExtraInfo, so low-level
// hooks can tell our own injected input apart from the physical devices.

#include <cstdint>

#include "kvm/protocol.h"

namespace kvm::input {

using MouseButton = kvm::proto::MouseButton;

struct Point {
    int x = 0;
    int y = 0;
};

// Value stored in dwExtraInfo of every event we inject ("KVM!").
inline constexpr std::uintptr_t kInjectedMarker = 0x4B564D21;

// True if a hook's dwExtraInfo came from one of our SendInput calls.
constexpr bool is_own_injection(std::uintptr_t extra_info) {
    return extra_info == kInjectedMarker;
}

// All send_* functions return false if Windows rejected the input, e.g. UIPI
// blocking injection into an elevated window, or a secure desktop being active.

// Relative move; subject to the local pointer speed / acceleration settings.
bool send_mouse_move_rel(int dx, int dy);

// Absolute move to (x, y) on the virtual desktop; clamped to its bounds.
bool move_cursor_abs(int x, int y);

bool send_mouse_button(MouseButton button, bool down);

// delta in WHEEL_DELTA (120) units per notch; positive = up / right.
bool send_wheel(int delta, bool horizontal);

// vk: virtual-key code. scan: hardware scan code without the E0/E1 prefix;
// pass 0 to derive it from vk. extended: the E0 prefix (arrows, right Ctrl/Alt,
// Insert/Delete/Home/End/PgUp/PgDn, numpad Enter and '/', Win keys...).
// If vk is 0 the key is injected by scan code only.
bool send_key(std::uint16_t vk, std::uint16_t scan, bool down, bool extended);

// Current cursor position in physical virtual-desktop pixels.
bool get_cursor_pos(Point& out);

}  // namespace kvm::input
