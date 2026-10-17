// Win32 window that shows the streamed frames from A (v1: GDI StretchDIBits).
// The image keeps its aspect ratio; unused space is black (letterboxing).
#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include <windows.h>

#include "jpeg_decoder.h"

namespace kvm {

// Largest rect with the image's aspect ratio that fits centered inside `area`.
RECT fit_rect(int image_w, int image_h, const RECT& area);

class ViewerWindow {
public:
    ViewerWindow() = default;
    ~ViewerWindow();

    ViewerWindow(const ViewerWindow&) = delete;
    ViewerWindow& operator=(const ViewerWindow&) = delete;

    // Creates and shows the window. The calling thread owns it and must run a message loop.
    // Closing the window posts WM_QUIT to that thread.
    bool create(const std::wstring& title, int client_w, int client_h);

    HWND hwnd() const { return hwnd_; }

    // Thread-safe (call from the video thread). Swaps `frame` with the pending one, so on return
    // `frame` holds a spare buffer — decode the next frame into it to avoid allocations. If the
    // previous frame was not painted yet, it is dropped: the window always shows the newest frame.
    void present(BgraImage& frame);

    // Thread-safe. Extra overlay line under the size/FPS line (e.g. decode time, latency).
    void set_status(std::wstring text);

private:
    struct FontDeleter {
        void operator()(HFONT font) const { DeleteObject(font); }
    };

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle_message(UINT msg, WPARAM wp, LPARAM lp);
    void paint();
    void update_font();

    HWND hwnd_ = nullptr;
    std::unique_ptr<HFONT__, FontDeleter> font_;

    // Three buffers: the video thread's spare, pending_ (newest, not yet painted) and displayed_.
    // paint() moves pending_ into displayed_ under the lock, then draws without it, so a slow paint
    // never blocks decoding of the next frame.
    std::mutex mutex_;  // guards pending_, has_pending_, status_, and hwnd_ for the video thread
    BgraImage pending_;
    bool has_pending_ = false;
    std::wstring status_;

    // UI thread only.
    BgraImage displayed_;
    int fps_ = 0;
    int frames_in_window_ = 0;
    std::chrono::steady_clock::time_point fps_window_start_ = std::chrono::steady_clock::now();
    double paint_ms_ = 0;
};

}  // namespace kvm
