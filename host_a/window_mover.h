// Moves one window of A onto the VDD monitor, maximized so it fills exactly what B shows, and back.
//
//   kvm::WindowMover mover;
//   if (mover.send(hwnd, vdd_rect) == kvm::WindowMover::Result::Ok) { ... stream it ... }
//   mover.restore(hwnd);   // before VirtualDisplay::detach(), or Windows parks it on the primary
//
// Coordinates are physical pixels: the process must be per-monitor DPI aware.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace kvm {

class WindowMover {
public:
    enum class Result {
        Ok,
        NotAWindow,
        Elevated,  // the window's process runs as admin; only an elevated kvm_host can move it (UIPI)
        Failed,
    };

    // Remembers where the window is, moves it onto `monitor` and maximizes it there.
    // On failure the window is left where it was.
    Result send(HWND hwnd, const RECT& monitor);

    // Puts the window back exactly as send() found it: position, size, maximized or minimized.
    // False if `hwnd` wasn't sent or has been closed meanwhile.
    bool restore(HWND hwnd);

    // The window that is on the VDD now, or nullptr.
    HWND sent_window() const { return hwnd_; }

private:
    HWND hwnd_ = nullptr;
    WINDOWPLACEMENT saved_{};
    RECT saved_rect_{};  // GetWindowRect before send(), to check that restore() really put it back
};

}  // namespace kvm
