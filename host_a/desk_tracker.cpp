// SPDX-License-Identifier: Apache-2.0

#include "desk_tracker.h"

#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <future>

#include "kvm/log.h"
#include "window_picker.h"

#pragma comment(lib, "dwmapi.lib")

namespace kvm {

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kMinGap = std::chrono::milliseconds(8);         // rescans after a burst of events
constexpr auto kRescanEvery = std::chrono::milliseconds(100);  // and anyway
// A window that opens this soon after a click on the other PC's screen is moved over there.
constexpr unsigned long long kOpenHereMs = 10'000;

// Set by the WinEvent callback, which runs on the tracker thread while it pumps messages.
thread_local bool t_dirty = false;
thread_local std::vector<HWND> t_shown;  // top-level windows that were just shown or uncloaked

bool is_top_level(HWND hwnd) {
    return GetAncestor(hwnd, GA_ROOT) == hwnd;
}

void CALLBACK on_win_event(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG id_object, LONG id_child, DWORD, DWORD) {
    if (!hwnd || id_object != OBJID_WINDOW || id_child != CHILDID_SELF) return;  // e.g. the cursor
    if (event == EVENT_OBJECT_LOCATIONCHANGE && !is_top_level(hwnd)) return;   // child controls
    t_dirty = true;
    if ((event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_UNCLOAKED) && is_top_level(hwnd)) t_shown.push_back(hwnd);
}

// Never part of the picture: the peer has its own desktop and taskbar there, and a dragged file's
// picture follows the cursor (the peer shows its own when the drag continues over there).
bool is_shell(const wchar_t* cls) {
    for (const wchar_t* shell :
         {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"SysShadow", L"SysDragImage"}) {
        if (std::wcscmp(cls, shell) == 0) return true;
    }
    return false;
}

display::Rect intersect(const display::Rect& a, const display::Rect& b) {
    display::Rect r{std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right),
                    std::min(a.bottom, b.bottom)};
    if (r.width() <= 0 || r.height() <= 0) return {};
    return r;
}

long long area(const display::Rect& r) {
    return static_cast<long long>(r.width()) * r.height();
}

bool cloaked(HWND hwnd) {
    DWORD value = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &value, sizeof(value))) && value != 0;
}

// The frame without the invisible resize borders and the shadow.
display::Rect frame_of(HWND hwnd) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(hwnd, &r);
    return {r.left, r.top, r.right, r.bottom};
}

std::string title_of(HWND hwnd) {
    std::wstring title(static_cast<size_t>(GetWindowTextLengthW(hwnd)) + 1, L'\0');
    title.resize(static_cast<size_t>(GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size()))));
    return to_utf8(title);
}

struct ScanContext {
    display::Rect vdd;
    DWORD own_pid = 0;
    std::vector<DeskTracker::Window> found;
};

BOOL CALLBACK collect(HWND hwnd, LPARAM lp) {
    auto& ctx = *reinterpret_cast<ScanContext*>(lp);
    if (ctx.found.size() >= desk::Region::kMaxRects) return FALSE;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return TRUE;
    if (intersect({wr.left, wr.top, wr.right, wr.bottom}, ctx.vdd).width() == 0) return TRUE;  // cheap test first

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx.own_pid) return TRUE;
    wchar_t cls[96]{};
    GetClassNameW(hwnd, cls, 96);
    if (is_shell(cls) || cloaked(hwnd)) return TRUE;
    const auto ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_LAYERED) {
        BYTE alpha = 255;
        DWORD flags = 0;
        if (GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0) {
            return TRUE;
        }
    }

    const display::Rect frame = frame_of(hwnd);
    const display::Rect visible = intersect(frame, ctx.vdd);
    if (visible.width() == 0) return TRUE;
    // Click-through overlays (game bars, screen tools) that cover the screen would swallow every click.
    if ((ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT) && area(visible) * 10 >= area(ctx.vdd) * 9) return TRUE;

    // Clicks belong to the window over its whole window rect, which takes in the invisible resize
    // borders just outside the visible frame: its edges can be dragged to resize it, as on a monitor.
    const display::Rect hit = intersect({wr.left, wr.top, wr.right, wr.bottom}, ctx.vdd);
    ctx.found.push_back({.hwnd = hwnd, .frame = frame, .rect = hit, .cls = to_utf8(cls), .title = title_of(hwnd)});
    return TRUE;
}

// An app's main window: not owned (dialogs open centred on their owner anyway), has a caption or a
// sizing frame, not a tool window, not part of the shell or of this process.
bool is_app_window(HWND hwnd) {
    if (!IsWindowVisible(hwnd) || !is_top_level(hwnd) || GetWindow(hwnd, GW_OWNER) || cloaked(hwnd)) return false;
    const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const auto ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) || ((style & WS_CAPTION) != WS_CAPTION && !(style & WS_THICKFRAME))) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return false;
    wchar_t cls[96]{};
    GetClassNameW(hwnd, cls, 96);
    return !is_shell(cls);
}

}  // namespace

DeskTracker::DeskTracker(desk::Region& region, Changed on_change) : region_(region), on_change_(std::move(on_change)) {
    std::promise<void> ready;
    auto started = ready.get_future();
    thread_ = std::thread([this, &ready] {
        thread_id_ = GetCurrentThreadId();
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // create the queue before anyone posts to it
        ready.set_value();
        run();
    });
    started.wait();
}

DeskTracker::~DeskTracker() {
    if (const DWORD id = thread_id_) PostThreadMessageW(id, WM_QUIT, 0, 0);
    if (thread_.joinable()) thread_.join();
}

std::vector<DeskTracker::Window> DeskTracker::windows() const {
    std::lock_guard lock(mutex_);
    return windows_;
}

void DeskTracker::run() {
    // Every window open now is "old": only windows that open from here on may be moved.
    EnumWindows(
        [](HWND hwnd, LPARAM lp) {
            reinterpret_cast<std::unordered_set<HWND>*>(lp)->insert(hwnd);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&known_));

    // Out of context: the callbacks run on this thread while it pumps messages.
    const HWINEVENTHOOK system_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, nullptr,
                                                      on_win_event, 0, 0, WINEVENT_OUTOFCONTEXT);
    const HWINEVENTHOOK object_hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_UNCLOAKED, nullptr,
                                                      on_win_event, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!system_hook || !object_hook) log::warn("desk: window events unavailable, rescanning every 100 ms");

    scan();
    auto last = Clock::now();
    for (;;) {
        const auto due = last + (t_dirty ? kMinGap : kRescanEvery);
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(due - Clock::now()).count();
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(std::max<long long>(wait, 0)), QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
        MSG msg;
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        place_new_windows();
        const auto now = Clock::now();
        if ((t_dirty && now - last >= kMinGap) || now - last >= kRescanEvery) {
            t_dirty = false;
            scan();
            last = now;
        }
    }
    if (system_hook) UnhookWinEvent(system_hook);
    if (object_hook) UnhookWinEvent(object_hook);
}

void DeskTracker::place_new_windows() {
    if (t_shown.empty()) return;
    std::vector<HWND> shown;
    shown.swap(t_shown);
    for (const HWND hwnd : shown) {
        if (!IsWindow(hwnd) || known_.contains(hwnd)) continue;
        if (!is_app_window(hwnd)) continue;  // not yet (e.g. still cloaked): it may come back uncloaked
        known_.insert(hwnd);
        display::Rect vdd;
        bool live = false;
        if (!region_.clicked_on_vdd_within(kOpenHereMs) || !region_.vdd_rect(vdd, live) || !live) continue;
        RECT wr{};
        GetWindowRect(hwnd, &wr);
        if (vdd.contains({(wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2})) continue;  // there already

        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(
            MonitorFromPoint({vdd.left + vdd.width() / 2, vdd.top + vdd.height() / 2}, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& work = mi.rcWork;
        const int w = std::min<int>(wr.right - wr.left, work.right - work.left);
        const int h = std::min<int>(wr.bottom - wr.top, work.bottom - work.top);
        const bool zoomed = IsZoomed(hwnd) != FALSE;
        // Asynchronous: a window that is still starting up must never stall this thread.
        if (zoomed) ShowWindowAsync(hwnd, SW_RESTORE);
        SetWindowPos(hwnd, nullptr, work.left + (work.right - work.left - w) / 2,
                     work.top + (work.bottom - work.top - h) / 2, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        if (zoomed) ShowWindowAsync(hwnd, SW_MAXIMIZE);
        log::info("desk: \"{}\" opened while working on the other screen - showing it there", title_of(hwnd));
    }
    if (known_.size() > 4096) {  // forget closed windows now and then
        std::erase_if(known_, [](HWND h) { return !IsWindow(h); });
    }
}

void DeskTracker::scan() {
    ScanContext ctx;
    bool live = false;
    if (region_.vdd_rect(ctx.vdd, live)) {
        ctx.own_pid = GetCurrentProcessId();
        EnumWindows(collect, reinterpret_cast<LPARAM>(&ctx));  // topmost first
    }
    std::vector<display::Rect> rects;
    rects.reserve(ctx.found.size());
    for (const auto& w : ctx.found) rects.push_back(w.rect);
    region_.set_rects(rects);

    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        const auto same_window = [](const Window& a, const Window& b) {
            return a.hwnd == b.hwnd && a.frame == b.frame && a.rect == b.rect;
        };
        changed = ctx.found.size() != windows_.size() ||
                  !std::equal(ctx.found.begin(), ctx.found.end(), windows_.begin(), same_window);
        const bool same_set = ctx.found.size() == windows_.size() &&
                              std::equal(ctx.found.begin(), ctx.found.end(), windows_.begin(),
                                         [](const Window& a, const Window& b) { return a.hwnd == b.hwnd; });
        if (!same_set) {
            std::string names;
            for (const auto& w : ctx.found) names += (names.empty() ? "\"" : ", \"") + w.title + "\"";
            log::info("desk: {} window(s) on the other screen{}{}", ctx.found.size(), names.empty() ? "" : ": ", names);
        }
        windows_ = ctx.found;
    }
    if (changed && on_change_) on_change_(ctx.found);
}

}  // namespace kvm
