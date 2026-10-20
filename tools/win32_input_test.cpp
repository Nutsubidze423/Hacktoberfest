// SPDX-License-Identifier: Apache-2.0

// Checks kvm/win32_input.h: installs low-level hooks, injects events and verifies
// what the hooks see (flags, the KVM marker, exact pixel for absolute moves).
// Clicks, keys and wheel are swallowed by the hook, so nothing reaches the desktop;
// only cursor moves go through (the cursor sweeps each monitor, then is restored).
// Usage: win32_input_test.exe   -- don't touch the mouse for ~5 s (exit code 0 = all passed)
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "kvm/dpi.h"
#include "kvm/win32_input.h"

using namespace kvm::input;

struct Ev {
    bool keyboard;
    WPARAM msg;
    DWORD a;      // mouse: mouseData   | key: vkCode
    DWORD b;      // mouse: flags       | key: scanCode
    DWORD flags;  // key flags
    ULONG_PTR extra;
    POINT pt;
};

static std::mutex g_mu;
static std::vector<Ev> g_events;
static std::atomic<bool> g_suppress{true};
static int g_fail = 0, g_pass = 0;

#define CHECK(cond, ...)                                   \
    do {                                                   \
        if (cond) { ++g_pass; }                            \
        else { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
               std::printf(__VA_ARGS__); std::printf("\n"); } \
    } while (0)

static LRESULT CALLBACK mouse_proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        if (is_own_injection(m->dwExtraInfo)) {
            { std::lock_guard l(g_mu); g_events.push_back({false, wp, m->mouseData, m->flags, 0, m->dwExtraInfo, m->pt}); }
            if (g_suppress && wp != WM_MOUSEMOVE) return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static LRESULT CALLBACK key_proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        if (is_own_injection(k->dwExtraInfo)) {
            { std::lock_guard l(g_mu); g_events.push_back({true, wp, k->vkCode, k->scanCode, k->flags, k->dwExtraInfo, {}}); }
            if (g_suppress) return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static std::vector<Ev> wait_events(size_t n, int ms = 1000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        { std::lock_guard l(g_mu); if (g_events.size() >= n) break; }
        Sleep(1);
    }
    Sleep(20);  // catch extras
    std::lock_guard l(g_mu);
    auto out = g_events;
    g_events.clear();
    return out;
}

static void clear_events() { Sleep(30); std::lock_guard l(g_mu); g_events.clear(); }

static bool wait_cursor(int x, int y, POINT& got) {
    for (int i = 0; i < 200; ++i) {
        GetCursorPos(&got);
        if (got.x == x && got.y == y) return true;
        Sleep(1);
    }
    return false;
}

static BOOL CALLBACK collect_monitor(HMONITOR, HDC, LPRECT r, LPARAM p) {
    reinterpret_cast<std::vector<RECT>*>(p)->push_back(*r);
    return TRUE;
}

int main() {
    if (!kvm::display::enable_dpi_awareness()) {
        std::printf("FATAL: process is not per-monitor DPI aware\n");
        return 2;
    }

    std::atomic<DWORD> hook_tid{0};
    std::atomic<bool> hooks_ok{false};
    std::thread hook_thread([&] {
        HHOOK hm = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, GetModuleHandleW(nullptr), 0);
        HHOOK hk = SetWindowsHookExW(WH_KEYBOARD_LL, key_proc, GetModuleHandleW(nullptr), 0);
        hooks_ok = hm && hk;
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
        hook_tid = GetCurrentThreadId();
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
        if (hm) UnhookWindowsHookEx(hm);
        if (hk) UnhookWindowsHookEx(hk);
    });
    while (!hook_tid) Sleep(1);
    if (!hooks_ok) { std::printf("FATAL: hooks not installed\n"); PostThreadMessageW(hook_tid, WM_QUIT, 0, 0); hook_thread.join(); return 2; }

    POINT orig{};
    GetCursorPos(&orig);

    // --- get_cursor_pos
    {
        Point p;
        CHECK(get_cursor_pos(p) && p.x == orig.x && p.y == orig.y, "get_cursor_pos %d,%d vs %ld,%ld", p.x, p.y, orig.x, orig.y);
    }

    // --- relative move
    {
        clear_events();
        CHECK(send_mouse_move_rel(5, -3), "send_mouse_move_rel returned false");
        auto ev = wait_events(1);
        CHECK(ev.size() == 1 && !ev[0].keyboard && ev[0].msg == WM_MOUSEMOVE, "rel move events=%zu", ev.size());
        if (!ev.empty()) CHECK((ev[0].b & LLMHF_INJECTED) && ev[0].extra == kInjectedMarker, "rel move not tagged");
        CHECK(send_mouse_move_rel(0, 0), "zero move should be a no-op success");
        CHECK(wait_events(1, 100).empty(), "zero move should inject nothing");
    }

    // --- absolute move: every pixel along each monitor's centre row and column
    {
        std::vector<RECT> mons;
        EnumDisplayMonitors(nullptr, nullptr, collect_monitor, reinterpret_cast<LPARAM>(&mons));
        std::printf("monitors: %zu, virtual desktop %d,%d %dx%d\n", mons.size(),
                    GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                    GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
        int abs_fail = 0, abs_total = 0;
        for (const RECT& r : mons) {
            std::printf("  monitor %ld,%ld - %ld,%ld\n", r.left, r.top, r.right, r.bottom);
            const int cy = (r.top + r.bottom) / 2, cx = (r.left + r.right) / 2;
            auto probe = [&](int x, int y) {
                ++abs_total;
                POINT got{};
                if (!move_cursor_abs(x, y) || !wait_cursor(x, y, got)) {
                    if (abs_fail++ < 10) std::printf("FAIL abs move to %d,%d -> %ld,%ld\n", x, y, got.x, got.y);
                }
            };
            for (int x = r.left; x < r.right; ++x) probe(x, cy);
            for (int y = r.top; y < r.bottom; ++y) probe(cx, y);
            probe(r.left, r.top); probe(r.right - 1, r.top);
            probe(r.left, r.bottom - 1); probe(r.right - 1, r.bottom - 1);
        }
        CHECK(abs_fail == 0, "abs moves: %d/%d wrong", abs_fail, abs_total);
        std::printf("abs moves checked: %d\n", abs_total);

        // Clamping: far outside the virtual desktop must still succeed.
        CHECK(move_cursor_abs(-100000, -100000), "clamp low");
        CHECK(move_cursor_abs(100000, 100000), "clamp high");
        clear_events();
    }
    SetCursorPos(orig.x, orig.y);
    clear_events();

    // --- buttons (suppressed)
    {
        struct { MouseButton b; WPARAM down, up; WORD xb; } cases[] = {
            {MouseButton::Left, WM_LBUTTONDOWN, WM_LBUTTONUP, 0},
            {MouseButton::Right, WM_RBUTTONDOWN, WM_RBUTTONUP, 0},
            {MouseButton::Middle, WM_MBUTTONDOWN, WM_MBUTTONUP, 0},
            {MouseButton::X1, WM_XBUTTONDOWN, WM_XBUTTONUP, XBUTTON1},
            {MouseButton::X2, WM_XBUTTONDOWN, WM_XBUTTONUP, XBUTTON2},
        };
        for (auto& c : cases) {
            CHECK(send_mouse_button(c.b, true) && send_mouse_button(c.b, false), "send_mouse_button false");
            auto ev = wait_events(2);
            bool ok = ev.size() == 2 && ev[0].msg == c.down && ev[1].msg == c.up &&
                      HIWORD(ev[0].a) == c.xb && HIWORD(ev[1].a) == c.xb &&
                      (ev[0].b & LLMHF_INJECTED) && ev[0].extra == kInjectedMarker;
            CHECK(ok, "button %d: events=%zu", int(c.b), ev.size());
        }
        CHECK(!send_mouse_button(static_cast<MouseButton>(42), true), "invalid button should fail");
    }

    // --- wheel (suppressed)
    {
        CHECK(send_wheel(120, false) && send_wheel(-240, false) && send_wheel(120, true) && send_wheel(-120, true), "send_wheel false");
        auto ev = wait_events(4);
        bool ok = ev.size() == 4 &&
                  ev[0].msg == WM_MOUSEWHEEL && short(HIWORD(ev[0].a)) == 120 &&
                  ev[1].msg == WM_MOUSEWHEEL && short(HIWORD(ev[1].a)) == -240 &&
                  ev[2].msg == WM_MOUSEHWHEEL && short(HIWORD(ev[2].a)) == 120 &&
                  ev[3].msg == WM_MOUSEHWHEEL && short(HIWORD(ev[3].a)) == -120;
        CHECK(ok, "wheel events=%zu", ev.size());
        CHECK(send_wheel(0, false), "zero wheel no-op");
        CHECK(wait_events(1, 100).empty(), "zero wheel injected something");
    }

    // --- keys (suppressed)
    {
        auto key_case = [](const char* name, WORD vk, WORD scan, bool ext,
                           DWORD want_vk, DWORD want_scan, bool want_ext) {
            CHECK(send_key(vk, scan, true, ext) && send_key(vk, scan, false, ext), "%s: send_key false", name);
            auto ev = wait_events(2);
            if (ev.size() != 2) { CHECK(false, "%s: events=%zu", name, ev.size()); return; }
            for (int i = 0; i < 2; ++i) {
                const bool up = i == 1;
                CHECK(ev[i].keyboard && ev[i].a == want_vk && ev[i].b == want_scan &&
                      bool(ev[i].flags & LLKHF_EXTENDED) == want_ext &&
                      bool(ev[i].flags & LLKHF_UP) == up &&
                      (ev[i].flags & LLKHF_INJECTED) && ev[i].extra == kInjectedMarker,
                      "%s %s: vk=%lx scan=%lx flags=%lx", name, up ? "up" : "down", ev[i].a, ev[i].b, ev[i].flags);
            }
        };
        key_case("A (scan derived)", 'A', 0, false, 'A', 0x1E, false);
        key_case("A (explicit scan)", 'A', 0x1E, false, 'A', 0x1E, false);
        key_case("Left (ext derived)", VK_LEFT, 0, false, VK_LEFT, 0x4B, true);
        key_case("Delete (ext derived)", VK_DELETE, 0, false, VK_DELETE, 0x53, true);
        key_case("PgDn (ext derived)", VK_NEXT, 0, false, VK_NEXT, 0x51, true);
        key_case("Numpad / (ext derived)", VK_DIVIDE, 0, false, VK_DIVIDE, 0x35, true);
        key_case("RAlt (ext derived)", VK_RMENU, 0, false, VK_RMENU, 0x38, true);
        key_case("Numpad 4 (not ext)", VK_NUMPAD4, 0, false, VK_NUMPAD4, 0x4B, false);
        key_case("Left explicit non-ext kept", VK_LEFT, 0x4B, false, VK_LEFT, 0x4B, false);
        key_case("RCtrl", VK_RCONTROL, 0x1D, true, VK_RCONTROL, 0x1D, true);
        key_case("LWin (ext derived)", VK_LWIN, 0, false, VK_LWIN, 0x5B, true);
        key_case("F13", VK_F13, 0, false, VK_F13, MapVirtualKeyW(VK_F13, MAPVK_VK_TO_VSC), false);
        key_case("scan-only 0x1E", 0, 0x1E, false, MapVirtualKeyW(0x1E, MAPVK_VSC_TO_VK), 0x1E, false);
        CHECK(!send_key(0, 0, true, false), "send_key(0,0) should fail");
    }

    SetCursorPos(orig.x, orig.y);
    PostThreadMessageW(hook_tid, WM_QUIT, 0, 0);
    hook_thread.join();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
