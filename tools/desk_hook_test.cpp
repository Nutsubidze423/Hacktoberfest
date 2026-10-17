// desk_hook_test: the KVM hooks in desk mode, on one PC with the real VDD attached on the right.
// The left half of the VDD plays "one of our windows" (Region rects); the right half is the peer's
// desktop. Checks: the cursor moves onto the VDD and desk_move follows it; clicks/wheel on the window
// stay here, elsewhere they go to the peer; the keyboard follows the last click; back on our screen
// the session ends; the peer taking over bounces the cursor home; with desk mode not live the cursor
// can't get lost on the VDD (and the old edge switch still works); Ctrl+Alt+Shift+Q ends it all.
//
// Input is faked with SendInput (accept_injected). An observer hook installed before the capture's
// sees what this PC itself would get, and swallows it, so nothing reaches the desktop.
// Usage: desk_hook_test.exe   -- don't touch mouse/keyboard for ~15 s (exit code 0 = all passed)

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kvm/desk_region.h"
#include "kvm/dpi.h"
#include "kvm/input_capture.h"
#include "virtual_display.h"
#include "window_picker.h"

namespace display = kvm::display;
namespace proto = kvm::proto;
using kvm::host::InputCapture;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) ++g_failures;
}

std::mutex g_mu;
std::vector<proto::Message> g_sent;  // what the peer receives

bool sink(const proto::Message& m) {
    std::lock_guard l(g_mu);
    g_sent.push_back(m);
    return true;
}

std::vector<proto::Message> take_sent() {
    std::lock_guard l(g_mu);
    return std::exchange(g_sent, {});
}

struct Seen {
    bool key;
    WPARAM msg;
};
std::vector<Seen> g_seen;  // what this PC itself receives (besides moves)

LRESULT CALLBACK observe_mouse(int code, WPARAM wp, LPARAM lp) {
    const auto& m = *reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
    if (code == HC_ACTION && m.dwExtraInfo == 0 && (m.flags & LLMHF_INJECTED) && wp != WM_MOUSEMOVE) {
        {
            std::lock_guard l(g_mu);
            g_seen.push_back({false, wp});
        }
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK observe_key(int code, WPARAM wp, LPARAM lp) {
    const auto& k = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
    if (code == HC_ACTION && k.dwExtraInfo == 0 && (k.flags & LLKHF_INJECTED)) {
        {
            std::lock_guard l(g_mu);
            g_seen.push_back({true, wp});
        }
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

std::vector<Seen> take_seen() {
    std::lock_guard l(g_mu);
    return std::exchange(g_seen, {});
}

void send(INPUT in) {
    SendInput(1, &in, sizeof(in));
}

void abs_move(display::Point p) {
    const auto vd = display::virtual_desktop();
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = static_cast<LONG>((2LL * (p.x - vd.left) + 1) * 32768 / vd.width());
    in.mi.dy = static_cast<LONG>((2LL * (p.y - vd.top) + 1) * 32768 / vd.height());
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    send(in);
}

void mouse(DWORD flags, DWORD data = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    send(in);
}

void key(WORD vk, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    send(in);
}

void settle() {
    Sleep(150);
}

bool wait_until(const std::function<bool()>& cond, int ms = 1500) {
    for (int i = 0; i < ms / 5; ++i) {
        if (cond()) return true;
        Sleep(5);
    }
    return cond();
}

template <class M>
std::vector<M> only(const std::vector<proto::Message>& msgs) {
    std::vector<M> out;
    for (const auto& m : msgs) {
        if (const auto* x = std::get_if<M>(&m)) out.push_back(*x);
    }
    return out;
}

display::Point cursor() {
    POINT c{};
    GetCursorPos(&c);
    return {c.x, c.y};
}

}  // namespace

int main() {
    std::thread([] {
        Sleep(60000);
        std::printf("\nwatchdog: test hung, exiting\n");
        TerminateProcess(GetCurrentProcess(), 3);
    }).detach();

    check(display::enable_dpi_awareness(), "dpi awareness");
    POINT orig{};
    GetCursorPos(&orig);

    auto vdd = kvm::VirtualDisplay::find();
    if (!vdd) {
        std::printf("the Virtual Display Driver is not installed\n");
        return 1;
    }
    const auto primary = display::primary_monitor().rect;
    if (!vdd->attach(primary.width(), primary.height(), 60, display::Side::Right)) {
        std::printf("could not attach the VDD\n");
        return 1;
    }
    const auto r = vdd->rect();
    const display::Rect v{r->rect.left, r->rect.top, r->rect.right, r->rect.bottom};
    std::printf("VDD at %d,%d %dx%d\n", v.left, v.top, v.width(), v.height());

    std::atomic<DWORD> obs_tid{0};
    std::thread observer([&] {
        HHOOK hm = SetWindowsHookExW(WH_MOUSE_LL, observe_mouse, GetModuleHandleW(nullptr), 0);
        HHOOK hk = SetWindowsHookExW(WH_KEYBOARD_LL, observe_key, GetModuleHandleW(nullptr), 0);
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
        obs_tid = hm && hk ? GetCurrentThreadId() : 1;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
        UnhookWindowsHookEx(hm);
        UnhookWindowsHookEx(hk);
    });
    while (!obs_tid) Sleep(1);

    kvm::desk::Region region;
    const std::string device = kvm::to_utf8(vdd->device());
    region.set_vdd({.device = device, .rect = v, .live = true});
    const display::Rect window{v.left, v.top, v.left + v.width() / 2, v.bottom};  // "our window" on the VDD
    region.set_rects(std::vector<display::Rect>{window});
    const display::Point on_window{v.left + v.width() / 4, v.top + v.height() / 2};
    const display::Point on_peer{v.left + v.width() * 3 / 4, v.top + v.height() / 2};
    const display::Point home{primary.left + primary.width() / 2, primary.top + primary.height() / 2};

    std::atomic<int> emergencies{0};
    {
        InputCapture cap({.client_side = display::Side::Right, .switch_cooldown_ms = 0, .accept_injected = true, .desk = &region});
        check(cap.start(), "hooks installed");
        cap.set_on_emergency([&] { ++emergencies; });
        cap.set_client(sink);
        abs_move(home);
        settle();
        take_sent();
        take_seen();

        // -- onto the VDD: desk session, the peer's cursor follows
        abs_move(on_peer);
        settle();
        {
            const auto sent = take_sent();
            const auto enters = only<proto::FocusEnter>(sent);
            const auto moves = only<proto::DeskMove>(sent);
            check(cap.remote() && cap.desk(), "moving onto the VDD starts a desk session");
            check(enters.size() == 1 && enters[0].desk, "focus_enter{desk} sent");
            check(!moves.empty() && moves.back().x == on_peer.x - v.left && moves.back().y == on_peer.y - v.top,
                  "desk_move carries the position on the peer's screen");
            const auto c = cursor();
            check(c.x == on_peer.x && c.y == on_peer.y, "the cursor really is on the VDD (like a third monitor)");
        }

        // -- a click on the peer's desktop goes to the peer, not here
        mouse(MOUSEEVENTF_LEFTDOWN);
        mouse(MOUSEEVENTF_LEFTUP);
        settle();
        {
            const auto buttons = only<proto::MouseButtonEvent>(take_sent());
            check(buttons.size() == 2 && buttons[0].down && !buttons[1].down, "click on the peer's desktop -> peer");
            check(take_seen().empty(), "... and not here");
        }
        // -- the keyboard follows that click
        key('A', true);
        key('A', false);
        settle();
        check(only<proto::Key>(take_sent()).size() == 2 && take_seen().empty(), "keys go to the peer after clicking there");
        mouse(MOUSEEVENTF_WHEEL, static_cast<DWORD>(-120));
        settle();
        check(only<proto::MouseWheel>(take_sent()).size() == 1 && take_seen().empty(), "wheel on the peer's desktop -> peer");

        // -- on our window: everything stays here
        abs_move(on_window);
        settle();
        {
            const auto moves = only<proto::DeskMove>(take_sent());
            check(!moves.empty() && moves.back().x == on_window.x - v.left, "desk_move follows onto our window");
        }
        mouse(MOUSEEVENTF_LEFTDOWN);
        mouse(MOUSEEVENTF_LEFTUP);
        settle();
        check(only<proto::MouseButtonEvent>(take_sent()).empty(), "click on our window is not sent");
        check(take_seen().size() == 2, "... it reaches our window here");
        key('B', true);
        key('B', false);
        settle();
        check(only<proto::Key>(take_sent()).empty() && take_seen().size() == 2, "keys stay here after clicking our window");
        mouse(MOUSEEVENTF_WHEEL, static_cast<DWORD>(120));
        settle();
        check(only<proto::MouseWheel>(take_sent()).empty() && take_seen().size() == 1, "wheel on our window stays here");

        // -- a button pressed on the peer's desktop is released there, wherever the cursor is
        abs_move(on_peer);
        settle();
        mouse(MOUSEEVENTF_RIGHTDOWN);
        abs_move(on_window);
        mouse(MOUSEEVENTF_RIGHTUP);
        settle();
        {
            const auto buttons = only<proto::MouseButtonEvent>(take_sent());
            check(buttons.size() == 2 && !buttons[1].down, "a press on the peer is released on the peer");
            check(take_seen().empty(), "... even if the cursor moved onto our window meanwhile");
        }

        // -- back on our screen: after a click here the session ends
        mouse(MOUSEEVENTF_LEFTDOWN);  // on our window: keyboard back here
        mouse(MOUSEEVENTF_LEFTUP);
        abs_move(home);
        settle();
        {
            const auto sent = take_sent();
            check(!cap.remote() && !cap.desk(), "back on the real monitor: session over");
            check(only<proto::FocusLeave>(sent).size() == 1, "focus_leave sent");
        }
        take_seen();

        // -- the peer takes its PC back while our cursor is over there: it comes home
        abs_move(on_peer);
        settle();
        take_sent();
        cap.on_focus_leave(proto::FocusLeave{.y_norm = 0.5});
        settle();
        check(!cap.remote(), "peer takeover ends the session");
        check(!v.contains(cursor()), "... and the cursor is back on our screen");
        check(only<proto::FocusLeave>(take_sent()).empty(), "... without telling the peer (it knows)");

        // -- desk mode not live: the cursor stops at the real edge, the old edge switch works there
        region.set_vdd({.device = device, .rect = v, .live = false});
        settle();
        abs_move(home);
        settle();
        take_sent();
        abs_move(on_peer);
        settle();
        {
            const auto c = cursor();
            check(!v.contains(c) && c.x == v.left - 1, "not live: the cursor can't go onto the VDD (stops at the edge)");
            const auto enters = only<proto::FocusEnter>(take_sent());
            check(enters.size() == 1 && !enters[0].desk && cap.remote() && !cap.desk(), "... where the old edge switch happens");
        }
        cap.switch_to_local(true);
        settle();
        take_sent();

        // -- the emergency exit in a desk session
        region.set_vdd({.device = device, .rect = v, .live = true});
        settle();
        abs_move(home);
        settle();
        abs_move(on_peer);
        settle();
        check(cap.desk(), "desk session again");
        key(VK_CONTROL, true);
        key(VK_MENU, true);
        key(VK_SHIFT, true);
        key('Q', true);
        key('Q', false);
        key(VK_SHIFT, false);
        key(VK_MENU, false);
        key(VK_CONTROL, false);
        check(wait_until([&] { return cap.disabled(); }), "Ctrl+Alt+Shift+Q disables the KVM");
        check(wait_until([&] { return emergencies.load() == 1; }), "... and calls the emergency callback");
        check(!v.contains(cursor()), "... and the cursor is back on our screen");
        cap.stop();
    }

    PostThreadMessageW(obs_tid, WM_QUIT, 0, 0);
    observer.join();
    vdd->detach();
    SetCursorPos(orig.x, orig.y);
    std::printf(g_failures == 0 ? "desk_hook_test: all checks passed\n" : "desk_hook_test: %d check(s) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
