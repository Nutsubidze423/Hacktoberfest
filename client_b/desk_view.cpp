#include "desk_view.h"

#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "kvm/log.h"
#include "kvm/win32_input.h"

namespace kvm {

namespace {

constexpr wchar_t kClassName[] = L"KvmDeskView";
constexpr UINT kMsgFrame = WM_APP + 1;
constexpr UINT kMsgClear = WM_APP + 2;

bool injected_by_us() {
    return input::is_own_injection(static_cast<std::uintptr_t>(GetMessageExtraInfo()));
}

RECT primary_rect() {
    const POINT origin{0, 0};
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcMonitor;
}

RECT bounds(const std::vector<RECT>& rects) {
    RECT b{};
    for (const auto& r : rects) {
        if (IsRectEmpty(&b)) {
            b = r;
        } else {
            UnionRect(&b, &b, &r);
        }
    }
    return b;
}

LPCWSTR cursor_id(std::string_view shape) {
    if (shape == "ibeam") return IDC_IBEAM;
    if (shape == "hand") return IDC_HAND;
    if (shape == "wait") return IDC_WAIT;
    if (shape == "appstarting") return IDC_APPSTARTING;
    if (shape == "cross") return IDC_CROSS;
    if (shape == "sizewe") return IDC_SIZEWE;
    if (shape == "sizens") return IDC_SIZENS;
    if (shape == "sizenwse") return IDC_SIZENWSE;
    if (shape == "sizenesw") return IDC_SIZENESW;
    if (shape == "sizeall") return IDC_SIZEALL;
    if (shape == "no") return IDC_NO;
    return IDC_ARROW;
}

}  // namespace

DeskView::DeskView() {
    cursor_ = LoadCursorW(nullptr, IDC_ARROW);
    input_.set_sink([this](const proto::Message& m) {
        Sink sink;
        {
            std::lock_guard lock(sink_mutex_);
            sink = sink_;
        }
        return sink && sink(m);
    });
    std::promise<void> ready;
    auto started = ready.get_future();
    ui_thread_ = std::thread([this, &ready] { ui_main(&ready); });
    started.wait();
    decode_thread_ = std::jthread([this] { decode_main(); });
}

DeskView::~DeskView() {
    incoming_.close();
    if (decode_thread_.joinable()) decode_thread_.join();
    if (const DWORD id = ui_thread_id_) PostThreadMessageW(id, WM_QUIT, 0, 0);
    if (ui_thread_.joinable()) ui_thread_.join();
}

void DeskView::present(net::Frame&& frame) {
    incoming_.put(std::move(frame));
}

void DeskView::clear() {
    if (const HWND h = hwnd_) PostMessageW(h, kMsgClear, 0, 0);
}

void DeskView::set_input_sink(Sink sink) {
    std::lock_guard lock(sink_mutex_);
    sink_ = std::move(sink);
}

void DeskView::set_cursor_shape(std::string_view shape) {
    cursor_ = LoadCursorW(nullptr, cursor_id(shape));
}

void DeskView::decode_main() {
    JpegDecoder decoder;
    while (auto frame = incoming_.take()) {
        Pending next{.header = std::move(frame->header)};
        if (next.header.codec == "jpeg" && !next.header.rects.empty()) {
            if (!decoder.decode(frame->payload, next.image)) {
                log::warn("desk: frame {} dropped: {}", next.header.seq, decoder.last_error());
                continue;
            }
        } else {
            next.header.rects.clear();  // codec "none": nothing to draw
        }
        {
            std::lock_guard lock(pending_mutex_);
            pending_ = std::move(next);
        }
        if (const HWND h = hwnd_) PostMessageW(h, kMsgFrame, 0, 0);
    }
}

void DeskView::ui_main(std::promise<void>* ready) {
    ui_thread_id_ = GetCurrentThreadId();
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &DeskView::wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);  // fails harmlessly if a second DeskView registered it already

    screen_ = primary_rect();
    const HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kClassName,
                                      L"Sarke desk", WS_POPUP, screen_.left, screen_.top, screen_.right - screen_.left,
                                      screen_.bottom - screen_.top, nullptr, nullptr, instance, this);
    if (!hwnd) log::error("desk: the overlay window could not be created ({})", GetLastError());
    hwnd_ = hwnd;
    if (hwnd && !make_surface()) log::error("desk: no drawing surface");
    ready->set_value();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    hwnd_ = nullptr;
    if (hwnd) DestroyWindow(hwnd);
    free_surface();
}

bool DeskView::make_surface() {
    free_surface();
    screen_ = primary_rect();
    const int w = screen_.right - screen_.left;
    const int h = screen_.bottom - screen_.top;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    mem_dc_ = CreateCompatibleDC(nullptr);
    bitmap_ = CreateDIBSection(mem_dc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!mem_dc_ || !bitmap_ || !bits) {
        free_surface();
        return false;
    }
    old_bitmap_ = SelectObject(mem_dc_, bitmap_);
    bits_ = static_cast<std::uint8_t*>(bits);
    std::memset(bits_, 0, static_cast<size_t>(w) * h * 4);  // fully transparent
    drawn_.clear();
    first_update_ = true;
    return true;
}

void DeskView::free_surface() {
    if (mem_dc_ && old_bitmap_) SelectObject(mem_dc_, old_bitmap_);
    if (bitmap_) DeleteObject(bitmap_);
    if (mem_dc_) DeleteDC(mem_dc_);
    mem_dc_ = nullptr;
    bitmap_ = nullptr;
    old_bitmap_ = nullptr;
    bits_ = nullptr;
}

void DeskView::show_pending() {
    std::optional<Pending> next;
    {
        std::lock_guard lock(pending_mutex_);
        next = std::exchange(pending_, std::nullopt);
    }
    const HWND hwnd = hwnd_;
    if (!next || !hwnd || !bits_) return;
    const auto& h = next->header;
    desk_w_ = h.desk_w;
    desk_h_ = h.desk_h;
    if (h.rects.empty()) {
        hide();
        return;
    }

    const int sw = screen_.right - screen_.left;
    const int sh = screen_.bottom - screen_.top;
    const size_t pitch = static_cast<size_t>(sw) * 4;
    // Clear what the previous frame drew, then copy the new rects out of the decoded crop.
    std::vector<RECT> old = std::move(drawn_);
    drawn_.clear();
    for (const auto& r : old) {
        for (int y = r.top; y < r.bottom; ++y) std::memset(bits_ + y * pitch + r.left * 4, 0, (r.right - r.left) * 4);
    }
    const BgraImage& img = next->image;
    for (const auto& d : h.rects) {
        const RECT r{std::max({d.x, h.x, 0}), std::max({d.y, h.y, 0}), std::min({d.x + d.w, h.x + img.width, sw}),
                     std::min({d.y + d.h, h.y + img.height, sh})};
        if (r.right <= r.left || r.bottom <= r.top) continue;
        for (int y = r.top; y < r.bottom; ++y) {
            std::memcpy(bits_ + y * pitch + r.left * 4,
                        img.pixels.data() + (y - h.y) * img.stride() + static_cast<size_t>(r.left - h.x) * 4,
                        static_cast<size_t>(r.right - r.left) * 4);  // decoded alpha is 255: already premultiplied
        }
        drawn_.push_back(r);
    }
    if (drawn_.empty()) {
        hide();
        return;
    }

    RECT dirty = bounds(old);
    const RECT now = bounds(drawn_);
    if (IsRectEmpty(&dirty)) {
        dirty = now;
    } else {
        UnionRect(&dirty, &dirty, &now);
    }
    POINT dst{screen_.left, screen_.top};
    SIZE size{sw, sh};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UPDATELAYEREDWINDOWINFO info{sizeof(info)};
    info.pptDst = &dst;
    info.psize = &size;
    info.hdcSrc = mem_dc_;
    info.pptSrc = &src;
    info.pblend = &blend;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = first_update_ ? nullptr : &dirty;
    if (!UpdateLayeredWindowIndirect(hwnd, &info)) {
        log::warn("desk: overlay update failed ({})", GetLastError());
        first_update_ = true;
        return;
    }
    first_update_ = false;
    ++shown_;

    // On top of everything, but never activated: clicks route by what the owner sees as its windows.
    const bool rects_changed =
        h.rects.size() != last_rects_.size() ||
        !std::equal(h.rects.begin(), h.rects.end(), last_rects_.begin(), [](const auto& a, const auto& b) {
            return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
        });
    if (!visible_ || rects_changed) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | (visible_ ? 0 : SWP_SHOWWINDOW));
        visible_ = true;
    }
    last_rects_ = h.rects;
}

void DeskView::hide() {
    if (buttons_held_ > 0 || GetCapture() == hwnd_) {
        ReleaseCapture();
        buttons_held_ = 0;
    }
    input_.release_all();
    if (bits_) {
        const size_t pitch = static_cast<size_t>(screen_.right - screen_.left) * 4;
        for (const auto& r : drawn_) {
            for (int y = r.top; y < r.bottom; ++y) std::memset(bits_ + y * pitch + r.left * 4, 0, (r.right - r.left) * 4);
        }
    }
    drawn_.clear();
    last_rects_.clear();
    first_update_ = true;
    if (visible_) {
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
    }
}

NormPoint DeskView::norm(LPARAM lp) const {
    // Client coordinates = screen coordinates of the primary monitor (the window covers it from 0,0).
    const double x = GET_X_LPARAM(lp) + (screen_.left);
    const double y = GET_Y_LPARAM(lp) + (screen_.top);
    return {desk_w_ > 0 ? x / desk_w_ : 0.0, desk_h_ > 0 ? y / desk_h_ : 0.0};
}

bool DeskView::on_input(UINT msg, WPARAM wp, LPARAM lp) {
    const auto button_of = [](UINT m, WPARAM w) -> std::optional<std::pair<proto::MouseButton, bool>> {
        switch (m) {
            case WM_LBUTTONDOWN: return std::pair{proto::MouseButton::Left, true};
            case WM_LBUTTONUP: return std::pair{proto::MouseButton::Left, false};
            case WM_RBUTTONDOWN: return std::pair{proto::MouseButton::Right, true};
            case WM_RBUTTONUP: return std::pair{proto::MouseButton::Right, false};
            case WM_MBUTTONDOWN: return std::pair{proto::MouseButton::Middle, true};
            case WM_MBUTTONUP: return std::pair{proto::MouseButton::Middle, false};
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
                return std::pair{GET_XBUTTON_WPARAM(w) == XBUTTON1 ? proto::MouseButton::X1 : proto::MouseButton::X2,
                                 m == WM_XBUTTONDOWN};
            default: return std::nullopt;
        }
    };

    switch (msg) {
        case WM_MOUSEMOVE:
            if (!injected_by_us() && desk_w_ > 0) input_.mouse_move(norm(lp));
            return true;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (!injected_by_us()) input_.wheel(GET_WHEEL_DELTA_WPARAM(wp), msg == WM_MOUSEHWHEEL);
            return true;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP:
            // Everything goes to the owner's window, Alt+F4 included (it closes that window, like on one PC).
            if (!injected_by_us()) input_.key(wp, lp, msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
            return true;
        default: break;
    }
    if (const auto b = button_of(msg, wp)) {
        if (injected_by_us() || desk_w_ <= 0) return true;
        if (b->second) {
            // Keys typed after this click belong to the owner's window too.
            SetForegroundWindow(hwnd_);
            SetFocus(hwnd_);
            if (buttons_held_++ == 0) SetCapture(hwnd_);
        } else if (buttons_held_ > 0 && --buttons_held_ == 0) {
            ReleaseCapture();
        }
        input_.mouse_button(b->first, b->second, norm(lp));
        return true;
    }
    return false;
}

LRESULT CALLBACK DeskView::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<DeskView*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<DeskView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT DeskView::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case kMsgFrame:
            show_pending();
            return 0;
        case kMsgClear:
            hide();
            return 0;
        case WM_DISPLAYCHANGE:
            hide();
            if (make_surface()) {
                SetWindowPos(hwnd, HWND_TOPMOST, screen_.left, screen_.top, screen_.right - screen_.left,
                             screen_.bottom - screen_.top, SWP_NOACTIVATE);
            }
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // a click doesn't activate it; on_input decides when it takes the keyboard
        case WM_SETCURSOR:
            SetCursor(cursor_.load());
            return TRUE;
        case WM_CAPTURECHANGED:
            if (buttons_held_ > 0 && reinterpret_cast<HWND>(lp) != hwnd) {
                buttons_held_ = 0;
                input_.release_all();
            }
            return 0;
        case WM_KILLFOCUS:
            input_.release_all();  // no stuck keys on the owner
            return 0;
        case WM_CLOSE:
            return 0;  // only the destructor closes it
        default: break;
    }
    if (on_input(msg, wp, lp)) return 0;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace kvm
