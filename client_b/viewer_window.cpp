#include "viewer_window.h"

#include <cstdint>
#include <format>
#include <mutex>
#include <type_traits>
#include <utility>

namespace kvm {

namespace {

constexpr wchar_t kClassName[] = L"KvmViewerWindow";
constexpr int kOverlayFontPoints = 10;

struct DcDeleter {
    void operator()(HDC dc) const { DeleteDC(dc); }
};
struct GdiObjectDeleter {
    void operator()(HGDIOBJ obj) const { DeleteObject(obj); }
};
using unique_dc = std::unique_ptr<std::remove_pointer_t<HDC>, DcDeleter>;
using unique_bitmap = std::unique_ptr<std::remove_pointer_t<HBITMAP>, GdiObjectDeleter>;

// Selects an object into a DC and restores the previous one on scope exit.
class SelectGuard {
public:
    SelectGuard(HDC dc, HGDIOBJ obj) : dc_(dc), old_(SelectObject(dc, obj)) {}
    ~SelectGuard() { SelectObject(dc_, old_); }
    SelectGuard(const SelectGuard&) = delete;
    SelectGuard& operator=(const SelectGuard&) = delete;

private:
    HDC dc_;
    HGDIOBJ old_;
};

bool register_class_once(WNDPROC wnd_proc) {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [wnd_proc] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        ok = RegisterClassExW(&wc) != 0;
    });
    return ok;
}

void draw_shadowed_text(HDC dc, const std::wstring& text, RECT rect) {
    SetBkMode(dc, TRANSPARENT);
    RECT shadow = rect;
    OffsetRect(&shadow, 1, 1);
    SetTextColor(dc, RGB(0, 0, 0));
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &shadow, DT_LEFT | DT_TOP | DT_NOPREFIX);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, DT_LEFT | DT_TOP | DT_NOPREFIX);
}

}  // namespace

RECT fit_rect(int image_w, int image_h, const RECT& area) {
    const int area_w = area.right - area.left;
    const int area_h = area.bottom - area.top;
    if (image_w <= 0 || image_h <= 0 || area_w <= 0 || area_h <= 0) {
        return {area.left, area.top, area.left, area.top};
    }
    int w = area_w;
    int h = static_cast<int>(static_cast<int64_t>(area_w) * image_h / image_w);
    if (h > area_h) {
        h = area_h;
        w = static_cast<int>(static_cast<int64_t>(area_h) * image_w / image_h);
    }
    const int x = area.left + (area_w - w) / 2;
    const int y = area.top + (area_h - h) / 2;
    return {x, y, x + w, y + h};
}

ViewerWindow::~ViewerWindow() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
}

bool ViewerWindow::create(const std::wstring& title, int client_w, int client_h) {
    if (!register_class_once(&ViewerWindow::wnd_proc)) {
        return false;
    }
    constexpr DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rect{0, 0, client_w, client_h};
    AdjustWindowRectExForDpi(&rect, style, FALSE, 0, GetDpiForSystem());

    const HWND hwnd = CreateWindowExW(0, kClassName, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                                      rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                      GetModuleHandleW(nullptr), this);
    if (!hwnd) {
        return false;
    }
    ShowWindow(hwnd, SW_SHOW);
    return true;
}

void ViewerWindow::present(BgraImage& frame) {
    std::lock_guard lock(mutex_);
    std::swap(pending_, frame);
    has_pending_ = true;
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);  // safe from any thread; WM_PAINT runs on the UI thread
    }
}

void ViewerWindow::set_status(std::wstring text) {
    std::lock_guard lock(mutex_);
    status_ = std::move(text);
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

LRESULT CALLBACK ViewerWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<ViewerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        {
            std::lock_guard lock(self->mutex_);
            self->hwnd_ = hwnd;
        }
        self->update_font();
    }
    auto* self = reinterpret_cast<ViewerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle_message(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT ViewerWindow::handle_message(UINT msg, WPARAM wp, LPARAM lp) {
    const HWND hwnd = hwnd_;
    switch (msg) {
    case WM_PAINT:
        paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;  // paint() covers the whole client area; skipping erase avoids flicker
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        update_font();
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY: {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        std::lock_guard lock(mutex_);
        hwnd_ = nullptr;
        break;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ViewerWindow::update_font() {
    const int height = -MulDiv(kOverlayFontPoints, static_cast<int>(GetDpiForWindow(hwnd_)), 72);
    font_.reset(CreateFontW(height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                            L"Segoe UI"));
}

void ViewerWindow::paint() {
    PAINTSTRUCT ps;
    const HDC hdc = BeginPaint(hwnd_, &ps);

    RECT client;
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    if (width > 0 && height > 0) {
        // Draw into an off-screen bitmap first, then copy once: no flicker from image + overlay.
        unique_dc mem(CreateCompatibleDC(hdc));
        unique_bitmap bitmap(CreateCompatibleBitmap(hdc, width, height));
        SelectGuard select_bitmap(mem.get(), bitmap.get());
        SelectGuard select_font(mem.get(), font_.get());

        FillRect(mem.get(), &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

        std::wstring status;
        {
            std::lock_guard lock(mutex_);
            if (has_pending_) {
                std::swap(pending_, displayed_);  // the old displayed buffer becomes the next spare
                has_pending_ = false;
                ++frames_in_window_;
            }
            status = status_;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = now - fps_window_start_;
        if (elapsed >= std::chrono::seconds(1)) {
            fps_ = static_cast<int>(frames_in_window_ * std::chrono::seconds(1) / elapsed);
            frames_in_window_ = 0;
            fps_window_start_ = now;
        }

        std::wstring overlay;
        if (!displayed_.empty()) {
            const RECT dst = fit_rect(displayed_.width, displayed_.height, client);

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = displayed_.width;
            bmi.bmiHeader.biHeight = -displayed_.height;  // negative = top-down rows
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            // HALFTONE averages pixels when shrinking, so text stays readable when scaled down.
            SetStretchBltMode(mem.get(), HALFTONE);
            SetBrushOrgEx(mem.get(), 0, 0, nullptr);
            const auto t0 = std::chrono::steady_clock::now();
            StretchDIBits(mem.get(), dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top, 0, 0,
                          displayed_.width, displayed_.height, displayed_.pixels.data(), &bmi, DIB_RGB_COLORS,
                          SRCCOPY);
            paint_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

            overlay = std::format(L"{}x{}  {} fps  draw {:.1f} ms", displayed_.width, displayed_.height, fps_,
                                  paint_ms_);
        } else {
            overlay = L"Waiting for stream...";
        }
        if (!status.empty()) {
            overlay += L"\n" + status;
        }

        const int margin = MulDiv(8, static_cast<int>(GetDpiForWindow(hwnd_)), 96);
        draw_shadowed_text(mem.get(), overlay, {margin, margin, width - margin, height - margin});

        BitBlt(hdc, 0, 0, width, height, mem.get(), 0, 0, SRCCOPY);
    }

    EndPaint(hwnd_, &ps);
}

}  // namespace kvm
