// Checks host_a/input_capture: edge switching, swallowing + forwarding while B
// has control, held keys/buttons, losing the client, and Ctrl+Alt+Shift+Q.
//
// Input is faked with SendInput and the capture runs with accept_injected. An
// "observer" hook installed before the capture runs after it, so whatever the
// observer sees is what A itself would have received. The observer swallows
// every fake click/key, so nothing reaches the desktop; only the cursor moves.
// Usage: input_capture_test.exe   -- don't touch mouse/keyboard for ~10 s (exit code 0 = all passed)

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "input_capture.h"
#include "kvm/dpi.h"
#include "kvm/win32_input.h"

namespace display = kvm::display;
namespace proto = kvm::proto;
using kvm::host::CaptureOptions;
using kvm::host::InputCapture;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) {
        ++g_failures;
    }
}

// ---- what B receives
std::mutex g_mu;
std::vector<proto::Message> g_sent;
std::atomic<bool> g_sink_ok{true};

bool sink(const proto::Message& m) {
    std::lock_guard l(g_mu);
    g_sent.push_back(m);
    return g_sink_ok;
}

std::vector<proto::Message> take_sent() {
    std::lock_guard l(g_mu);
    return std::exchange(g_sent, {});
}

// ---- what A itself receives (observer hook)
struct Seen {
    bool key;
    WPARAM msg;
    DWORD code;  // vk for keys
};
std::vector<Seen> g_seen;

LRESULT CALLBACK observe_mouse(int code, WPARAM wp, LPARAM lp) {
    const auto& m = *reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
    if (code == HC_ACTION && m.dwExtraInfo == 0 && (m.flags & LLMHF_INJECTED) && wp != WM_MOUSEMOVE) {
        { std::lock_guard l(g_mu); g_seen.push_back({false, wp, 0}); }
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK observe_key(int code, WPARAM wp, LPARAM lp) {
    const auto& k = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
    if (code == HC_ACTION && k.dwExtraInfo == 0 && (k.flags & LLKHF_INJECTED)) {
        { std::lock_guard l(g_mu); g_seen.push_back({true, wp, k.vkCode}); }
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

std::vector<Seen> take_seen() {
    std::lock_guard l(g_mu);
    return std::exchange(g_seen, {});
}

// ---- fake "physical" input (dwExtraInfo = 0, not our marker)
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

void rel_move(int dx, int dy) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
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
    Sleep(120);
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

bool cursor_at(display::Point p) {
    POINT c{};
    GetCursorPos(&c);
    return c.x == p.x && c.y == p.y;
}

}  // namespace

int main() {
    // Watchdog: whatever happens, the hooks die with the process.
    std::thread([] { Sleep(40000); std::printf("\nwatchdog: test hung, exiting\n"); TerminateProcess(GetCurrentProcess(), 3); }).detach();

    check(display::enable_dpi_awareness(), "dpi awareness");
    POINT orig{};
    GetCursorPos(&orig);

    // Observer hooks first, so the capture's hooks (installed later) run before them.
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

    const auto mons = display::monitors();
    const display::OuterEdge right(mons, display::Side::Right);
    const auto edge_at = [&](double t) { return right.point(t); };
    const display::Point inside{mons.front().rect.left + mons.front().rect.width() / 2,
                                mons.front().rect.top + mons.front().rect.height() / 2};

    {
        InputCapture cap({.client_side = display::Side::Right, .accept_injected = true});
        check(cap.start(), "start(): hooks installed");
        InputCapture second({.client_side = display::Side::Right});
        check(!second.start(), "a second capture in the same process is refused");

        // -- no client: the edge does nothing
        abs_move(inside); settle();
        abs_move(edge_at(0.5)); settle();
        check(!cap.remote() && take_sent().empty(), "no client attached: edge does not switch");

        cap.set_client(sink);

        // -- our own injected moves are never treated as physical
        abs_move(inside); settle();
        kvm::input::move_cursor_abs(edge_at(0.5).x, edge_at(0.5).y); settle();
        check(!cap.remote(), "own injected move to the edge (KVM marker) is ignored");

        // -- edge switch
        abs_move(inside); settle();
        abs_move(edge_at(0.25));
        check(wait_until([&] { return cap.remote(); }), "cursor hits the right edge -> control on B");
        settle();
        {
            const auto enters = only<proto::FocusEnter>(take_sent());
            check(enters.size() == 1 && std::abs(enters[0].y_norm - right.position(edge_at(0.25)).value()) < 1e-9,
                  "focus_enter sent once with the crossing point");
        }
        take_seen();

        // -- while on B: everything swallowed and forwarded
        rel_move(7, -3); rel_move(5, 1); settle();
        mouse(MOUSEEVENTF_LEFTDOWN); mouse(MOUSEEVENTF_LEFTUP);
        mouse(MOUSEEVENTF_XDOWN, XBUTTON2); mouse(MOUSEEVENTF_XUP, XBUTTON2);
        mouse(MOUSEEVENTF_WHEEL, 120); mouse(MOUSEEVENTF_HWHEEL, static_cast<DWORD>(-240));
        key(VK_F13, true); key(VK_F13, false);
        settle();
        {
            const auto sent = take_sent();
            int dx = 0, dy = 0;
            for (const auto& m : only<proto::MouseMove>(sent)) dx += m.dx, dy += m.dy;
            check(dx == 12 && dy == -2, "raw input motion forwarded (dx " + std::to_string(dx) + ", dy " + std::to_string(dy) + ")");
            const auto buttons = only<proto::MouseButtonEvent>(sent);
            check(buttons.size() == 4 && buttons[0].button == proto::MouseButton::Left && buttons[0].down &&
                      !buttons[1].down && buttons[2].button == proto::MouseButton::X2 && buttons[2].down && !buttons[3].down,
                  "buttons forwarded (left, x2)");
            const auto wheels = only<proto::MouseWheel>(sent);
            check(wheels.size() == 2 && wheels[0].delta == 120 && !wheels[0].horizontal && wheels[1].delta == -240 &&
                      wheels[1].horizontal,
                  "wheel forwarded (vertical, horizontal)");
            const auto keys = only<proto::Key>(sent);
            check(keys.size() == 2 && keys[0].vk == VK_F13 && keys[0].down && !keys[1].down &&
                      keys[0].scan == MapVirtualKeyW(VK_F13, MAPVK_VK_TO_VSC),
                  "keys forwarded with scan code");
            check(take_seen().empty(), "A itself saw none of it (all swallowed)");
        }

        // -- B hands control back
        cap.on_focus_leave({.y_norm = 0.75});
        check(wait_until([&] { return !cap.remote(); }), "focus_leave from B -> control on A");
        const auto back = display::inward(edge_at(0.75), display::Side::Right, 2);
        check(wait_until([&] { return cursor_at(back); }), "cursor placed 2 px inside A's edge at B's y");
        check(only<proto::FocusLeave>(take_sent()).empty(), "B's own focus_leave is not echoed back");

        // -- local input reaches A again
        key(VK_F14, true); settle();
        check(take_seen().size() == 1, "local mode: keys reach A");

        // -- F14 is still held when control goes to B: its release belongs to A
        abs_move(edge_at(0.5));
        check(wait_until([&] { return cap.remote(); }), "switch to B while a key is held");
        key(VK_F14, false); settle();
        {
            const auto seen = take_seen();
            check(seen.size() == 1 && seen[0].key && seen[0].code == VK_F14 && seen[0].msg == WM_KEYUP,
                  "release of a key held before switching goes to A");
            check(only<proto::Key>(take_sent()).empty(), "...and not to B");
        }

        // -- a held mouse button blocks switching (no lost drag)
        cap.on_focus_leave({.y_norm = 0.5});
        wait_until([&] { return !cap.remote(); });
        mouse(MOUSEEVENTF_LEFTDOWN); settle();
        abs_move(edge_at(0.5)); settle();
        check(!cap.remote(), "left button held: edge does not switch");
        mouse(MOUSEEVENTF_LEFTUP); settle();
        abs_move(inside); settle();
        abs_move(edge_at(0.5));
        check(wait_until([&] { return cap.remote(); }), "button released: edge switches");
        take_sent();
        take_seen();

        // -- send failure brings control back
        g_sink_ok = false;
        key(VK_F13, true);
        check(wait_until([&] { return !cap.remote(); }), "send to B fails -> control on A");
        g_sink_ok = true;
        key(VK_F13, false);
        settle();
        take_sent();
        take_seen();

        // -- programmatic switch and A taking control back
        cap.switch_to_remote(0.7);
        check(wait_until([&] { return cap.remote(); }), "switch_to_remote()");
        cap.switch_to_local();
        check(wait_until([&] { return !cap.remote(); }), "switch_to_local()");
        settle();
        {
            const auto sent = take_sent();
            const auto enters = only<proto::FocusEnter>(sent);
            check(enters.size() == 1 && enters[0].y_norm == 0.7, "focus_enter carries the requested y_norm");
            check(only<proto::FocusLeave>(sent).size() == 1, "A taking control back tells B (focus_leave)");
        }

        // -- client goes away while on B
        cap.switch_to_remote(0.5);
        wait_until([&] { return cap.remote(); });
        cap.set_client(nullptr);
        check(wait_until([&] { return !cap.remote(); }), "client detached while on B -> control on A");
        abs_move(inside); settle();
        abs_move(edge_at(0.5)); settle();
        check(!cap.remote(), "no client: no switching");
        cap.set_client(sink);
        take_sent();

        // -- emergency exit while B has control
        cap.switch_to_remote(0.5);
        wait_until([&] { return cap.remote(); });
        take_seen();
        key(VK_LCONTROL, true); key(VK_LMENU, true); key(VK_LSHIFT, true); key('Q', true);
        check(wait_until([&] { return cap.disabled(); }), "Ctrl+Alt+Shift+Q -> disabled");
        check(!cap.remote(), "...control on A");
        settle();
        check(only<proto::FocusLeave>(take_sent()).size() == 1, "...B told to let go (focus_leave)");
        check(take_seen().empty(), "...the Q never reached A");
        key('Q', false); key(VK_LSHIFT, false); key(VK_LMENU, false); key(VK_LCONTROL, false);
        key(VK_F15, true); key(VK_F15, false); settle();
        check(take_seen().size() == 6, "...hooks removed: every key now reaches A");
        abs_move(inside); settle();
        abs_move(edge_at(0.5)); settle();
        check(!cap.remote() && take_sent().empty(), "...and the edge no longer switches");

        cap.stop();
        cap.stop();
        check(true, "stop() twice is fine");
    }

    // -- edge switching off: only switch_to_remote() switches
    {
        InputCapture cap({.client_side = display::Side::Right, .edge_switching = false, .accept_injected = true});
        check(cap.start(), "restart with edge_switching = false");
        cap.set_client(sink);
        abs_move(inside); settle();
        abs_move(edge_at(0.5)); settle();
        check(!cap.remote(), "edge ignored when edge_switching = false");
        cap.switch_to_remote(0.5);
        check(wait_until([&] { return cap.remote(); }), "switch_to_remote() still works");
    }
    check(true, "destructor stops a capture that is on B");

    // -- touchpad regression: precision touchpads deliver Raw Input with a null
    //    hDevice. SendInput motion looks the same in Raw Input, so with
    //    accept_injected = false it must still be forwarded while on B.
    {
        InputCapture cap({.client_side = display::Side::Right});
        check(cap.start(), "start with default options (accept_injected = false)");
        cap.set_client(sink);
        cap.switch_to_remote(0.5);
        check(wait_until([&] { return cap.remote(); }), "on B");
        take_sent();
        rel_move(9, 4); rel_move(3, -1); settle();
        int dx = 0, dy = 0;
        for (const auto& m : only<proto::MouseMove>(take_sent())) dx += m.dx, dy += m.dy;
        check(dx == 12 && dy == 3, "Raw Input with a null device (touchpad) is forwarded (dx " + std::to_string(dx) +
                                       ", dy " + std::to_string(dy) + ")");
    }

    // -- no Raw Input motion: motion comes from the hook, cursor parked mid-monitor
    {
        InputCapture cap({.client_side = display::Side::Right, .accept_injected = true, .raw_input = false});
        check(cap.start(), "start with raw_input = false");
        cap.set_client(sink);
        abs_move(inside); settle();
        const auto exit_at = edge_at(0.4);
        abs_move(exit_at);
        check(wait_until([&] { return cap.remote(); }), "edge -> B");
        const auto& r = mons.front().rect;
        const display::Point park{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        check(wait_until([&] { return cursor_at(park); }), "cursor parked mid-monitor (the edge move was swallowed)");
        take_sent();
        rel_move(10, 0); rel_move(10, 0); rel_move(0, -10); settle();
        int dx = 0, dy = 0;
        for (const auto& m : only<proto::MouseMove>(take_sent())) dx += m.dx, dy += m.dy;
        check(dx > 0 && dy < 0, "hook motion forwarded (dx " + std::to_string(dx) + ", dy " + std::to_string(dy) + ")");
        check(cursor_at(park), "cursor stays parked while on B");
        cap.switch_to_local();
        const auto back = display::inward(exit_at, display::Side::Right, 2);
        check(wait_until([&] { return cursor_at(back); }), "A takes control back: cursor returns to where it left");
    }

    SetCursorPos(orig.x, orig.y);
    PostThreadMessageW(obs_tid, WM_QUIT, 0, 0);
    observer.join();
    std::printf("\n%s\n", g_failures == 0 ? "all passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
