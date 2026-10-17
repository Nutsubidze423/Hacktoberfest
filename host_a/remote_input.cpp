#include "remote_input.h"

#include <algorithm>
#include <cmath>
#include <variant>

#include "kvm/log.h"
#include "kvm/win32_input.h"

namespace kvm {

RemoteInput::RemoteInput(HWND window, const RECT& target) : window_(window), target_(target) {
    GetWindowThreadProcessId(window_, &window_pid_);
}

RemoteInput::~RemoteInput() {
    release_all();
}

bool RemoteInput::handle(const proto::Message& msg) {
    return std::visit(proto::Overloaded{
                          [this](const proto::RemoteMouseMove& m) { handle(m); return true; },
                          [this](const proto::MouseButtonEvent& m) { handle(m); return true; },
                          [this](const proto::MouseWheel& m) { handle(m); return true; },
                          [this](const proto::Key& m) { handle(m); return true; },
                          [](const auto&) { return false; },
                      },
                      msg);
}

POINT RemoteInput::to_screen(double x_norm, double y_norm) const {
    const int w = target_.right - target_.left, h = target_.bottom - target_.top;
    const auto along = [](double norm, int size) {
        return static_cast<int>(std::lround(std::clamp(norm, 0.0, 1.0) * (size - 1)));
    };
    return {target_.left + along(x_norm, w), target_.top + along(y_norm, h)};
}

void RemoteInput::handle(const proto::RemoteMouseMove& m) {
    const POINT p = to_screen(m.x_norm, m.y_norm);
    input::move_cursor_abs(p.x, p.y);
}

void RemoteInput::handle(const proto::MouseButtonEvent& m) {
    // The click lands where the viewer clicked, even if no move arrived right before it. Move only
    // if the cursor is elsewhere: a move with the button down on the title bar starts a drag, and
    // dragging a maximized window un-maximizes it.
    const POINT p = to_screen(m.x_norm, m.y_norm);
    input::Point now;
    if (!input::get_cursor_pos(now) || std::abs(now.x - p.x) > 1 || std::abs(now.y - p.y) > 1) {
        input::move_cursor_abs(p.x, p.y);
    }
    if (m.down) ensure_foreground();  // so the first click acts instead of only activating
    if (input::send_mouse_button(m.button, m.down)) {
        buttons_down_[static_cast<size_t>(m.button)] = m.down;
    }
    if (!m.down) keep_parked();
}

void RemoteInput::keep_parked() {
    if (!IsWindow(window_)) return;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    const HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONULL);
    const bool on_target = monitor && GetMonitorInfoW(monitor, &info) && EqualRect(&info.rcMonitor, &target_);
    if (on_target && IsZoomed(window_)) return;
    // The viewer shows the whole VDD, so the window must fill it: a click or drag on its title bar
    // (or a double click) un-maximized or moved it. Its original place for restore is untouched.
    if (IsZoomed(window_)) ShowWindow(window_, SW_RESTORE);
    SetWindowPos(window_, nullptr, target_.left, target_.top, target_.right - target_.left,
                 target_.bottom - target_.top, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(window_, SW_MAXIMIZE);
    log::info("remote_input: the window was un-maximized or moved; it fills the VDD again");
}

void RemoteInput::handle(const proto::MouseWheel& m) {
    input::send_wheel(m.delta, m.horizontal);  // at the cursor, which the last move put on the window
}

bool RemoteInput::handle(const proto::Key& m) {
    if (m.vk == VK_LWIN || m.vk == VK_RWIN) return false;
    if (m.down) ensure_foreground();
    if (!input::send_key(m.vk, m.scan, m.down, m.extended)) return false;
    if (m.vk < keys_down_.size()) keys_down_[m.vk] = m.down;
    return true;
}

void RemoteInput::release_all() {
    for (size_t vk = 0; vk < keys_down_.size(); ++vk) {
        if (keys_down_[vk]) input::send_key(static_cast<std::uint16_t>(vk), 0, false, false);
    }
    for (size_t b = 0; b < buttons_down_.size(); ++b) {
        if (buttons_down_[b]) input::send_mouse_button(static_cast<proto::MouseButton>(b), false);
    }
    keys_down_.reset();
    buttons_down_.reset();
}

bool RemoteInput::ensure_foreground() {
    const HWND foreground = GetForegroundWindow();
    DWORD foreground_pid = 0;
    const DWORD foreground_thread = foreground ? GetWindowThreadProcessId(foreground, &foreground_pid) : 0;
    // One of the app's own windows (a dialog, a menu) already has the keyboard: leave it there.
    if (foreground == window_ || (foreground_pid != 0 && foreground_pid == window_pid_)) return true;

    // Only the foreground window's thread may hand the foreground over, so borrow its input queue.
    const DWORD me = GetCurrentThreadId();
    const bool attached = foreground_thread != 0 && foreground_thread != me &&
                          AttachThreadInput(me, foreground_thread, TRUE);
    const bool ok = SetForegroundWindow(window_) != FALSE;
    if (attached) AttachThreadInput(me, foreground_thread, FALSE);
    if (!ok) log::warn("remote_input: couldn't bring the shared window to the foreground");
    return ok;
}

}  // namespace kvm
