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
constexpr auto kMinGap = std::chrono::milliseconds(8);       // rescans after a burst of events
constexpr auto kRescanEvery = std::chrono::milliseconds(100);  // and anyway

// Set by the WinEvent callback, which runs on the tracker thread while it pumps messages.
thread_local bool t_dirty = false;

void CALLBACK on_win_event(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG id_object, LONG id_child, DWORD, DWORD) {
    if (!hwnd || id_object != OBJID_WINDOW || id_child != CHILDID_SELF) return;  // e.g. the cursor
    if (event == EVENT_OBJECT_LOCATIONCHANGE && GetAncestor(hwnd, GA_ROOT) != hwnd) return;  // child controls
    t_dirty = true;
}

// Never part of the picture: the peer has its own desktop and taskbar there.
bool is_shell(const wchar_t* cls) {
    for (const wchar_t* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"SysShadow"}) {
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
    if (is_shell(cls)) return TRUE;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    const auto ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_LAYERED) {
        BYTE alpha = 255;
        DWORD flags = 0;
        if (GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0) {
            return TRUE;
        }
    }

    // The frame without the invisible resize borders and the shadow.
    RECT fr{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof(fr)))) fr = wr;
    const display::Rect visible = intersect({fr.left, fr.top, fr.right, fr.bottom}, ctx.vdd);
    if (visible.width() == 0) return TRUE;
    // Click-through overlays (game bars, screen tools) that cover the screen would swallow every click.
    if ((ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT) && area(visible) * 10 >= area(ctx.vdd) * 9) return TRUE;

    std::wstring title(static_cast<size_t>(GetWindowTextLengthW(hwnd)) + 1, L'\0');
    title.resize(static_cast<size_t>(GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size()))));
    ctx.found.push_back({.hwnd = hwnd, .rect = visible, .cls = to_utf8(cls), .title = to_utf8(title)});
    return TRUE;
}

}  // namespace

DeskTracker::DeskTracker(desk::Region& region) : region_(region) {
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

    std::lock_guard lock(mutex_);
    const bool same = ctx.found.size() == windows_.size() &&
                      std::equal(ctx.found.begin(), ctx.found.end(), windows_.begin(),
                                 [](const Window& a, const Window& b) { return a.hwnd == b.hwnd; });
    if (!same) {
        std::string names;
        for (const auto& w : ctx.found) names += (names.empty() ? "\"" : ", \"") + w.title + "\"";
        log::info("desk: {} window(s) on the other screen{}{}", ctx.found.size(), names.empty() ? "" : ": ", names);
    }
    windows_ = std::move(ctx.found);
}

}  // namespace kvm
