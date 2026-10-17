#include "window_mover.h"

#include <memory>
#include <optional>
#include <vector>

#include "kvm/log.h"

namespace kvm {
namespace {

struct HandleCloser {
    void operator()(HANDLE h) const noexcept {
        if (h) CloseHandle(h);
    }
};
using unique_handle = std::unique_ptr<void, HandleCloser>;

std::optional<DWORD> token_integrity(HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    if (size == 0) return std::nullopt;
    std::vector<std::byte> buffer(size);
    if (!GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size)) return std::nullopt;
    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
    const UCHAR count = *GetSidSubAuthorityCount(label->Label.Sid);
    return *GetSidSubAuthority(label->Label.Sid, count - 1);
}

// UIPI: a process can't move or resize windows of a process with a higher integrity level
// (e.g. an app started "as administrator" while kvm_host runs normally).
bool owned_by_higher_integrity(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    unique_handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    HANDLE raw_token = nullptr;
    if (!process || !OpenProcessToken(process.get(), TOKEN_QUERY, &raw_token)) {
        // Elevated processes refuse to show their token to a normal one.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    unique_handle token{raw_token};
    const auto theirs = token_integrity(token.get());
    const auto mine = token_integrity(GetCurrentProcessToken());
    return theirs && mine && *theirs > *mine;
}

bool on_monitor(HWND hwnd, const RECT& monitor) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    const HMONITOR m = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    return m && GetMonitorInfoW(m, &info) && EqualRect(&info.rcMonitor, &monitor);
}

// After a window crosses to a monitor with another DPI, its app resizes it on its own thread
// (WM_DPICHANGED), a moment after our call returned. Wait until its rect stops changing.
void wait_until_settled(HWND hwnd) {
    RECT last{};
    GetWindowRect(hwnd, &last);
    int stable = 0;
    for (int i = 0; i < 30 && stable < 3; ++i) {  // at most 1.5 s, settled after 150 ms unchanged
        Sleep(50);
        RECT now{};
        GetWindowRect(hwnd, &now);
        stable = EqualRect(&now, &last) ? stable + 1 : 0;
        last = now;
    }
}

bool placement_matches(HWND hwnd, const WINDOWPLACEMENT& expected) {
    WINDOWPLACEMENT now{};
    now.length = sizeof(now);
    return GetWindowPlacement(hwnd, &now) && now.showCmd == expected.showCmd &&
           EqualRect(&now.rcNormalPosition, &expected.rcNormalPosition);
}

}  // namespace

WindowMover::Result WindowMover::send(HWND hwnd, const RECT& monitor) {
    if (!IsWindow(hwnd)) return Result::NotAWindow;
    if (owned_by_higher_integrity(hwnd)) {
        log::error("window_mover: the window belongs to an elevated process; run kvm_host as admin");
        return Result::Elevated;
    }

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    RECT rect{};
    if (!GetWindowPlacement(hwnd, &placement) || !GetWindowRect(hwnd, &rect)) {
        log::error("window_mover: can't read the window position ({})", GetLastError());
        return Result::Failed;
    }

    // A maximized window can't be moved and a minimized one has no real position: restore it first.
    // (Restoring a minimized window that was maximized gives a maximized one, hence two rounds.)
    for (int i = 0; i < 2 && (IsIconic(hwnd) || IsZoomed(hwnd)); ++i) {
        ShowWindow(hwnd, SW_RESTORE);
    }

    // Move first, so a monitor with a different DPI rescales the window, then maximize on it.
    const int width = monitor.right - monitor.left;
    const int height = monitor.bottom - monitor.top;
    if (!SetWindowPos(hwnd, nullptr, monitor.left, monitor.top, width, height, SWP_NOZORDER | SWP_NOACTIVATE)) {
        const DWORD error = GetLastError();
        SetWindowPlacement(hwnd, &placement);
        if (error == ERROR_ACCESS_DENIED) {
            log::error("window_mover: access denied moving the window; run kvm_host as admin");
            return Result::Elevated;
        }
        log::error("window_mover: SetWindowPos failed ({})", error);
        return Result::Failed;
    }
    wait_until_settled(hwnd);
    ShowWindow(hwnd, SW_MAXIMIZE);

    if (!on_monitor(hwnd, monitor) || !IsZoomed(hwnd)) {
        log::error("window_mover: the window did not end up maximized on the target monitor");
        SetWindowPlacement(hwnd, &placement);
        return Result::Failed;
    }
    hwnd_ = hwnd;
    saved_ = placement;
    saved_rect_ = rect;
    return Result::Ok;
}

bool WindowMover::restore(HWND hwnd) {
    if (!hwnd || hwnd != hwnd_) return false;
    hwnd_ = nullptr;
    if (!IsWindow(hwnd)) return false;  // closed while it was on B

    // A window that is maximized on the VDD would stay there: SetWindowPlacement only updates its
    // saved normal position. Un-maximize it first.
    if (IsZoomed(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
        wait_until_settled(hwnd);
    }
    // The first call brings the window back to its monitor, where the app rescales it for that
    // monitor's DPI on top of the size we set. The next call is on the same monitor, so it sticks.
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!SetWindowPlacement(hwnd, &saved_)) {
            log::error("window_mover: SetWindowPlacement failed ({})", GetLastError());
            return false;
        }
        wait_until_settled(hwnd);
        RECT now{};
        GetWindowRect(hwnd, &now);
        if (placement_matches(hwnd, saved_) && EqualRect(&now, &saved_rect_)) return true;
    }
    log::warn("window_mover: the window is back, but not exactly where it was");
    return true;
}

}  // namespace kvm
