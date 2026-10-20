// SPDX-License-Identifier: Apache-2.0

// Checks client_b/input_inject: feeds it the messages A would send and verifies
// what it injects (via a low-level hook) and what it sends back to A.
// The hook swallows every injected click/key, so nothing reaches the desktop;
// only the cursor moves (restored at the end).
// Usage: input_inject_test.exe   -- don't touch the mouse for ~5 s (exit code 0 = all passed)

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kvm/input_inject.h"
#include "kvm/dpi.h"
#include "kvm/win32_input.h"

namespace display = kvm::display;
namespace proto = kvm::proto;
using kvm::client::InputInject;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) {
        ++g_failures;
    }
}

std::mutex g_mu;
std::vector<proto::Message> g_to_host;

struct Seen {
    bool key;
    WPARAM msg;
    DWORD code;  // vk for keys
};
std::vector<Seen> g_seen;

LRESULT CALLBACK observe_mouse(int code, WPARAM wp, LPARAM lp) {
    const auto& m = *reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
    if (code == HC_ACTION && kvm::input::is_own_injection(m.dwExtraInfo) && wp != WM_MOUSEMOVE) {
        { std::lock_guard l(g_mu); g_seen.push_back({false, wp, 0}); }
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK observe_key(int code, WPARAM wp, LPARAM lp) {
    const auto& k = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
    if (code == HC_ACTION && kvm::input::is_own_injection(k.dwExtraInfo)) {
        { std::lock_guard l(g_mu); g_seen.push_back({true, wp, k.vkCode}); }
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

std::vector<Seen> take_seen() {
    Sleep(80);
    std::lock_guard l(g_mu);
    return std::exchange(g_seen, {});
}

std::vector<proto::Message> take_to_host() {
    std::lock_guard l(g_mu);
    return std::exchange(g_to_host, {});
}

display::Point cursor() {
    POINT c{};
    GetCursorPos(&c);
    return {c.x, c.y};
}

bool wait_cursor(const std::function<bool(display::Point)>& cond) {
    for (int i = 0; i < 300; ++i) {
        if (cond(cursor())) return true;
        Sleep(2);
    }
    return false;
}

int count(const std::vector<Seen>& seen, bool key, WPARAM msg, DWORD code = 0) {
    int n = 0;
    for (const Seen& s : seen) n += s.key == key && s.msg == msg && (!key || s.code == code);
    return n;
}

}  // namespace

int main() {
    std::thread([] { Sleep(30000); std::printf("\nwatchdog: test hung, exiting\n"); TerminateProcess(GetCurrentProcess(), 3); }).detach();
    check(display::enable_dpi_awareness(), "dpi awareness");
    const display::Point orig = cursor();

    std::atomic<DWORD> tid{0};
    std::thread observer([&] {
        HHOOK hm = SetWindowsHookExW(WH_MOUSE_LL, observe_mouse, GetModuleHandleW(nullptr), 0);
        HHOOK hk = SetWindowsHookExW(WH_KEYBOARD_LL, observe_key, GetModuleHandleW(nullptr), 0);
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
        tid = GetCurrentThreadId();
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
        UnhookWindowsHookEx(hm);
        UnhookWindowsHookEx(hk);
    });
    while (!tid) Sleep(1);

    // B stands to the right of A, so A is on B's left.
    const auto mons = display::monitors();
    const display::OuterEdge left(mons, display::Side::Left);
    const auto to_host = [](const proto::Message& m) {
        std::lock_guard l(g_mu);
        g_to_host.push_back(m);
        return true;
    };
    InputInject inject(display::Side::Left, to_host, std::chrono::milliseconds(0));

    // -- not B's turn yet: input is ignored
    const display::Point before = cursor();
    check(inject.handle(proto::MouseMove{.dx = 40, .dy = 0}), "mouse_move is ours");
    Sleep(50);
    check(!inject.active() && cursor().x == before.x, "inactive: mouse_move ignored");
    check(!inject.handle(proto::Hello{}) && !inject.handle(proto::Ping{}), "other message types are not ours");

    // -- focus_enter: cursor on the edge facing A, 2 px in
    inject.handle(proto::FocusEnter{.y_norm = 0.5});
    const auto entry = display::inward(left.point(0.5), display::Side::Left, 2);
    check(inject.active(), "focus_enter -> active");
    check(wait_cursor([&](display::Point p) { return p.x == entry.x && p.y == entry.y; }),
          "cursor placed at (" + std::to_string(entry.x) + ", " + std::to_string(entry.y) + ")");

    // -- relative motion
    inject.handle(proto::MouseMove{.dx = 60, .dy = 0});
    check(wait_cursor([&](display::Point p) { return p.x > entry.x; }), "mouse_move moves the cursor away from A");

    // -- buttons, wheel, keys get injected
    inject.handle(proto::MouseButtonEvent{.button = proto::MouseButton::Left, .down = true});
    inject.handle(proto::MouseWheel{.delta = -120, .horizontal = false});
    inject.handle(proto::MouseWheel{.delta = 120, .horizontal = true});
    inject.handle(proto::Key{.vk = VK_LSHIFT, .scan = 0x2A, .down = true});
    inject.handle(proto::Key{.vk = VK_F13, .scan = 0x64, .down = true});
    inject.handle(proto::Key{.vk = VK_F13, .scan = 0x64, .down = false});
    inject.handle(proto::Key{.vk = VK_F14, .scan = 0x65, .down = true});
    {
        const auto seen = take_seen();
        check(count(seen, false, WM_LBUTTONDOWN) == 1, "button down injected");
        check(count(seen, false, WM_MOUSEWHEEL) == 1 && count(seen, false, WM_MOUSEHWHEEL) == 1, "wheel injected (both axes)");
        check(count(seen, true, WM_KEYDOWN, VK_LSHIFT) == 1 && count(seen, true, WM_KEYDOWN, VK_F13) == 1 &&
                  count(seen, true, WM_KEYUP, VK_F13) == 1 && count(seen, true, WM_KEYDOWN, VK_F14) == 1,
              "keys injected");
    }

    // -- moving along the edge or away from A does not leave
    inject.handle(proto::MouseMove{.dx = -100000, .dy = 0});
    check(wait_cursor([&](display::Point p) { return left.position(p).has_value(); }), "big move left: cursor on the edge facing A");
    inject.handle(proto::MouseMove{.dx = 0, .dy = 5});
    Sleep(30);
    check(inject.active() && take_to_host().empty(), "moving along the edge stays on B");

    // -- pushing on towards A hands control back and releases everything held
    const display::Point at_edge = cursor();
    inject.handle(proto::MouseMove{.dx = -3, .dy = 0});
    check(!inject.active(), "pushing past the edge -> control back to A");
    {
        const auto sent = take_to_host();
        const auto* leave = sent.size() == 1 ? std::get_if<proto::FocusLeave>(&sent[0]) : nullptr;
        check(leave && std::abs(leave->y_norm - left.position(at_edge).value()) < 1e-9, "focus_leave sent with the exit point");
        const auto seen = take_seen();
        check(count(seen, false, WM_LBUTTONUP) == 1, "held button released");
        check(count(seen, true, WM_KEYUP, VK_LSHIFT) == 1 && count(seen, true, WM_KEYUP, VK_F14) == 1 &&
                  count(seen, true, WM_KEYUP, VK_F13) == 0,
              "held keys released (and only those)");
    }
    const display::Point after = cursor();
    inject.handle(proto::MouseMove{.dx = 50, .dy = 0});
    inject.handle(proto::Key{.vk = VK_F13, .scan = 0x64, .down = true});
    Sleep(50);
    check(cursor().x == after.x && take_seen().empty(), "after leaving: input from A ignored");

    // -- A takes control back (emergency / A-side switch)
    inject.handle(proto::FocusEnter{.y_norm = 0.0});
    const auto top = display::inward(left.point(0.0), display::Side::Left, 2);
    check(wait_cursor([&](display::Point p) { return p.x == top.x && p.y == top.y; }), "focus_enter y_norm 0 -> top of the edge");
    inject.handle(proto::Key{.vk = VK_RCONTROL, .scan = 0x1D, .down = true, .extended = true});
    inject.handle(proto::MouseButtonEvent{.button = proto::MouseButton::X1, .down = true});
    take_seen();
    inject.handle(proto::FocusLeave{.y_norm = 0.5});
    check(!inject.active(), "focus_leave from A -> inactive");
    {
        const auto seen = take_seen();
        check(count(seen, true, WM_KEYUP, VK_RCONTROL) == 1 && count(seen, false, WM_XBUTTONUP) == 1,
              "A taking over releases held keys/buttons");
        check(take_to_host().empty(), "...without echoing focus_leave back");
    }

    // -- disconnect
    inject.handle(proto::FocusEnter{.y_norm = 1.0});
    inject.handle(proto::Key{.vk = 'A', .scan = 0x1E, .down = true});
    take_seen();
    inject.release_all();
    check(!inject.active() && count(take_seen(), true, WM_KEYUP, 'A') == 1, "release_all() on disconnect");

    // -- cooldown: right after focus_enter, the edge facing A doesn't hand control back
    {
        InputInject cooled(display::Side::Left, to_host);  // default 250 ms
        cooled.handle(proto::FocusEnter{.y_norm = 0.5});
        cooled.handle(proto::MouseMove{.dx = -100000, .dy = 0});
        wait_cursor([&](display::Point p) { return left.position(p).has_value(); });
        cooled.handle(proto::MouseMove{.dx = -3, .dy = 0});
        check(cooled.active() && take_to_host().empty(), "push towards A within the cooldown: stays on B");
        Sleep(300);
        cooled.handle(proto::MouseMove{.dx = -3, .dy = 0});
        check(!cooled.active() && take_to_host().size() == 1, "push after the cooldown: control back to A");
    }

    SetCursorPos(orig.x, orig.y);
    PostThreadMessageW(tid, WM_QUIT, 0, 0);
    observer.join();
    std::printf("\n%s\n", g_failures == 0 ? "all passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
