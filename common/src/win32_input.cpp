// SPDX-License-Identifier: Apache-2.0

#include "kvm/win32_input.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>

namespace kvm::input {
namespace {

INPUT make_mouse(DWORD flags, LONG dx = 0, LONG dy = 0, DWORD data = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.mouseData = data;
    in.mi.dwFlags = flags;
    in.mi.dwExtraInfo = kInjectedMarker;
    return in;
}

bool send_one(INPUT& in) {
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

struct VirtualDesktop {
    int left, top, width, height;
};

VirtualDesktop virtual_desktop() {
    return {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

// Maps a pixel offset to SendInput's 0..65535 absolute range, aiming at the
// pixel's centre so rounding inside Windows always lands on the same pixel.
LONG to_absolute(int offset, int extent) {
    const long long v = (2LL * offset + 1) * 32768 / extent;
    return static_cast<LONG>(std::clamp(v, 0LL, 65535LL));
}

// Keys whose scan code needs the E0 prefix but which MapVirtualKey reports
// without it, because they share the scan code with a numpad key.
bool is_always_extended(std::uint16_t vk) {
    switch (vk) {
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_RCONTROL:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
    case VK_APPS:
    case VK_DIVIDE:
    case VK_NUMLOCK:
    case VK_SNAPSHOT:
    case VK_CANCEL: return true;
    default: return false;
    }
}

}  // namespace

bool send_mouse_move_rel(int dx, int dy) {
    if (dx == 0 && dy == 0) return true;
    INPUT in = make_mouse(MOUSEEVENTF_MOVE, dx, dy);
    return send_one(in);
}

bool move_cursor_abs(int x, int y) {
    const VirtualDesktop vd = virtual_desktop();
    if (vd.width <= 0 || vd.height <= 0) return false;
    const int ox = std::clamp(x - vd.left, 0, vd.width - 1);
    const int oy = std::clamp(y - vd.top, 0, vd.height - 1);
    INPUT in = make_mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, to_absolute(ox, vd.width),
                          to_absolute(oy, vd.height));
    return send_one(in);
}

bool send_mouse_button(MouseButton button, bool down) {
    DWORD flags = 0;
    DWORD data = 0;
    switch (button) {
    case MouseButton::Left: flags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
    case MouseButton::Right: flags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
    case MouseButton::Middle: flags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    case MouseButton::X1:
    case MouseButton::X2:
        flags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        data = button == MouseButton::X1 ? XBUTTON1 : XBUTTON2;
        break;
    default: return false;
    }
    INPUT in = make_mouse(flags, 0, 0, data);
    return send_one(in);
}

bool send_wheel(int delta, bool horizontal) {
    if (delta == 0) return true;
    // mouseData is a DWORD but carries a signed delta.
    INPUT in = make_mouse(horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(delta));
    return send_one(in);
}

bool send_key(std::uint16_t vk, std::uint16_t scan, bool down, bool extended) {
    if (vk == 0 && scan == 0) return false;
    if (scan == 0) {
        // High byte is the prefix: 0xE0 = extended, 0xE1 = Pause.
        const UINT ex = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
        scan = static_cast<std::uint16_t>(ex & 0xFF);
        extended = extended || (ex >> 8) == 0xE0 || is_always_extended(vk);
    }

    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = scan;
    // With a VK the target sees both vk and scan; without one, Windows maps
    // the scan code through the local layout.
    in.ki.dwFlags =
        (vk == 0 ? KEYEVENTF_SCANCODE : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    in.ki.dwExtraInfo = kInjectedMarker;
    return send_one(in);
}

bool get_cursor_pos(Point& out) {
    POINT p{};
    if (!GetCursorPos(&p)) return false;
    out = {p.x, p.y};
    return true;
}

}  // namespace kvm::input
