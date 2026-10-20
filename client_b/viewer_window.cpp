// SPDX-License-Identifier: Apache-2.0

#include "viewer_window.h"

#include <windowsx.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <numeric>
#include <string_view>
#include <utility>
#include <vector>

#include "kvm/dpi.h"
#include "kvm/log.h"
#include "kvm/net.h"

#pragma comment(lib, "dwmapi.lib")

namespace kvm {

namespace {

namespace vt = ui::viewer;
using ui::pal;
using ui::px;
using ui::TextStyle;

constexpr wchar_t kClassName[] = L"KvmViewerWindow";
constexpr UINT kMsgStreamStart = WM_APP + 1;  // wParam = width, lParam = height
constexpr UINT kMsgLinkChanged = WM_APP + 2;  // wParam = LinkState before, lParam = LinkState now
constexpr UINT_PTR kHudTimer = 1;
constexpr UINT_PTR kBlinkTimer = 2;

bool register_class_once(WNDPROC wnd_proc) {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [wnd_proc] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        ok = RegisterClassExW(&wc) != 0;
    });
    return ok;
}

std::wstring widen(const std::string& s) {
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

bool reduced_motion() {
    BOOL animate = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
    return !animate;
}

D2D1_RECT_F to_rect_f(const RECT& r) {
    return {static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right),
            static_cast<float>(r.bottom)};
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

bool ViewerWindow::create(bool visible) {
    try {
        ui::Graphics::get();
    } catch (const std::exception& e) {
        log::error("viewer: {}", e.what());
        return false;
    }
    if (!register_class_once(&ViewerWindow::wnd_proc)) {
        return false;
    }

    // Footer: B's own info, from the same source as the hello message.
    wchar_t name[256];
    DWORD name_len = static_cast<DWORD>(std::size(name));
    if (!GetComputerNameExW(ComputerNameDnsHostname, name, &name_len)) {
        name_len = 0;
    }
    const auto primary = display::primary_monitor();
    footer_ = std::format(L"{}{}{}{}{}{}{}%", std::wstring_view(name, name_len), ui::kSeparator, primary.rect.width(),
                          ui::kTimes, primary.rect.height(), ui::kSeparator, std::lround(primary.scale * 100));
    reduced_motion_ = reduced_motion();

    const UINT system_dpi = GetDpiForSystem();
    const HWND hwnd = CreateWindowExW(0, kClassName, ui::kAppName, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                      px(vt::initial_client_w, system_dpi), px(vt::initial_client_h, system_dpi),
                                      nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd) {
        return false;
    }
    apply_dpi(GetDpiForWindow(hwnd));
    update_icons();

    ui::style_window(hwnd);  // rounded corners, no system border, theme-matched shadow

    // Real size at the window's own DPI; SWP_FRAMECHANGED applies the frameless WM_NCCALCSIZE.
    SetWindowPos(hwnd, nullptr, 0, 0, px(vt::initial_client_w, dpi_), px(vt::initial_client_h, dpi_),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);

    SetTimer(hwnd, kHudTimer, vt::hud_update_ms, nullptr);
    const UINT blink_ms = GetCaretBlinkTime();
    if (!reduced_motion_ && blink_ms != 0 && blink_ms != INFINITE) {
        SetTimer(hwnd, kBlinkTimer, blink_ms, nullptr);
    }
    if (visible) {
        ShowWindow(hwnd, SW_SHOW);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Thread-safe setters

void ViewerWindow::set_endpoint(const std::string& host, std::uint16_t port) {
    std::lock_guard lock(mutex_);
    shared_.host = host;
    shared_.port = port;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void ViewerWindow::set_reconnects(bool reconnects) {
    std::lock_guard lock(mutex_);
    shared_.reconnects = reconnects;
}

void ViewerWindow::set_link_state(LinkState state) {
    std::lock_guard lock(mutex_);
    if (shared_.link == state) {
        return;
    }
    if (shared_.link == LinkState::Linked) {
        ++shared_.generation;  // the stream ends with the link
        shared_.has_pending = false;
        shared_.stream_title.clear();
        shared_.stream_hwnd.reset();
    }
    const LinkState before = shared_.link;
    shared_.link = state;
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        PostMessageW(hwnd_, kMsgLinkChanged, static_cast<WPARAM>(before), static_cast<LPARAM>(state));
    }
}

void ViewerWindow::bring_to_front() {
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void ViewerWindow::update_icons() {
    small_icon_ = ui::create_logo_icon(ui::small_icon_size(dpi_));
    big_icon_ = ui::create_logo_icon(ui::large_icon_size(dpi_));
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon_.get()));
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon_.get()));
}

void ViewerWindow::set_rtt_ms(std::optional<int> rtt_ms) {
    std::lock_guard lock(mutex_);
    shared_.rtt_ms = rtt_ms;
}

void ViewerWindow::set_jpeg_quality(std::optional<int> quality) {
    std::lock_guard lock(mutex_);
    shared_.jpeg_quality = quality;
}

void ViewerWindow::start_stream(const proto::StreamStart& start) {
    std::lock_guard lock(mutex_);
    ++shared_.generation;
    shared_.has_pending = false;
    shared_.stream_title = widen(start.title);  // UTF-8 on the wire
    shared_.stream_hwnd = start.hwnd;
    if (hwnd_) {
        PostMessageW(hwnd_, kMsgStreamStart, static_cast<WPARAM>(start.width), static_cast<LPARAM>(start.height));
    }
}

void ViewerWindow::stop_stream() {
    std::lock_guard lock(mutex_);
    ++shared_.generation;
    shared_.has_pending = false;
    shared_.stream_title.clear();
    shared_.stream_hwnd.reset();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void ViewerWindow::give_back_stream() {
    std::optional<std::uint64_t> owner_hwnd;
    {
        std::lock_guard lock(mutex_);
        owner_hwnd = shared_.stream_hwnd;
    }
    if (!owner_hwnd) {
        return;  // nothing is being streamed to us
    }
    input_.release_all();  // nothing stays pressed in the owner's window
    if (!sink_ || !sink_(proto::StreamStop{.hwnd = *owner_hwnd})) {
        log::warn("viewer: could not send stream_stop (give the window back)");
    }
    stop_stream();  // the owner confirms with its own stream_stop
}

void ViewerWindow::present(BgraImage& frame, const FrameInfo& info) {
    std::lock_guard lock(mutex_);
    std::swap(shared_.pending, frame);
    shared_.pending_info = info;
    shared_.has_pending = true;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);  // safe from any thread
}

// ---------------------------------------------------------------------------------------------
// UI thread

std::optional<D2D1_POINT_2F> ViewerWindow::to_normalized(POINT client, bool clamp) const {
    const D2D1_RECT_F& r = image_rect_;
    if (!have_frame_ || r.right <= r.left || r.bottom <= r.top) {
        return std::nullopt;
    }
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const auto strip_bottom = static_cast<float>(px(vt::strip_h, dpi_));
    // Visible part of the image only: never the strip, the letterbox or a cropped (1:1) edge.
    const float left = std::max(r.left, 0.0f);
    const float right = std::min(r.right, static_cast<float>(rc.right));
    const float top = std::max(r.top, strip_bottom);
    const float bottom = std::min(r.bottom, static_cast<float>(rc.bottom));
    if (right <= left || bottom <= top) {
        return std::nullopt;
    }
    float x = static_cast<float>(client.x) + 0.5f;  // pixel center
    float y = static_cast<float>(client.y) + 0.5f;
    if (clamp) {
        x = std::clamp(x, left + 0.5f, right - 0.5f);
        y = std::clamp(y, top + 0.5f, bottom - 0.5f);
    } else if (x < left || x >= right || y < top || y >= bottom) {
        return std::nullopt;
    }
    return D2D1::Point2F((x - r.left) / (r.right - r.left), (y - r.top) / (r.bottom - r.top));
}

bool ViewerWindow::input_active() const {
    const View view = current_view();
    return view == View::Streaming || view == View::Stalled;
}

void ViewerWindow::on_mouse_button(proto::MouseButton button, bool down, LPARAM lp) {
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (down) {
        if (!input_active()) {
            return;
        }
        const auto p = to_normalized(pt);
        if (!p) {
            return;  // letterbox: ignored, not sent
        }
        if (!input_.any_button_held()) {
            SetCapture(hwnd_);  // keep getting the drag when it leaves the image or the window
        }
        input_.mouse_button(button, true, {p->x, p->y});
        return;
    }
    if (!input_.button_held(button)) {
        return;
    }
    if (const auto p = to_normalized(pt, true)) {
        input_.mouse_button(button, false, {p->x, p->y});
    } else {
        input_.release_all();  // the image went away mid-drag
    }
    if (!input_.any_button_held()) {
        ReleaseCapture();
    }
}

LRESULT CALLBACK ViewerWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<ViewerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        std::lock_guard lock(self->mutex_);
        self->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<ViewerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle_message(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT ViewerWindow::handle_message(UINT msg, WPARAM wp, LPARAM lp) {
    const HWND hwnd = hwnd_;
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp == TRUE) {
            // No standard frame: the client covers the whole window (DWM shadow, Snap, resize stay).
            if (IsZoomed(hwnd)) {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                const int padded = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
                const int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi_) + padded;
                const int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi_) + padded;
                params->rgrc[0].left += fx;
                params->rgrc[0].right -= fx;
                params->rgrc[0].top += fy;
                params->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST:
        return hit_test_nc({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
    case WM_PAINT:
        render();
        ValidateRect(hwnd, nullptr);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (rt_) {
            rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {px(vt::min_client_w, dpi_), px(vt::min_client_h, dpi_)};
        return 0;
    }
    case WM_DPICHANGED: {
        apply_dpi(HIWORD(wp));
        update_icons();
        const auto* suggested = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        reduced_motion_ = reduced_motion();
        if (reduced_motion_) {
            blink_on_ = true;
        }
        if (ui::is_theme_change(msg, lp)) {  // Windows switched between dark and light apps
            ui::refresh_theme();
            ui::style_window(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_TIMER:
        if (wp == kHudTimer) {
            on_hud_tick();
        } else if (wp == kBlinkTimer) {
            on_blink_tick();
        }
        return 0;
    case kMsgLinkChanged:
        if (link_listener_) {
            link_listener_(static_cast<LinkState>(wp), static_cast<LinkState>(lp));
        }
        return 0;
    case kMsgStreamStart:
        take_pending_frame();  // syncs the new stream generation first
        stream_sized_ = true;
        resize_for_stream(static_cast<int>(wp), static_cast<int>(lp));
        if (!IsWindowVisible(hwnd)) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);  // a hidden viewer appears without stealing focus
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        give_back_stream();
        if (close_handler_) {
            close_handler_();
            return 0;
        }
        break;  // DefWindowProc destroys the window -> WM_DESTROY -> WM_QUIT
    case WM_MOUSEMOVE: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!tracking_mouse_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            tracking_mouse_ = TrackMouseEvent(&tme) != FALSE;
        }
        if (input_.any_button_held()) {
            // Dragging on A: follow the pointer even outside the image (clamped to its edge).
            if (const auto p = to_normalized(pt, true)) {
                input_.mouse_move({p->x, p->y});
            }
            return 0;
        }
        RECT rc;
        GetClientRect(hwnd, &rc);
        const auto button = viewer::hit_test(viewer::layout_strip(static_cast<float>(rc.right), dpi_, text_),
                                             static_cast<float>(pt.x), static_cast<float>(pt.y));
        if (button != hover_) {
            hover_ = button;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (button == viewer::StripButton::None && pressed_ == viewer::StripButton::None && input_active()) {
            if (const auto p = to_normalized(pt)) {
                input_.mouse_move({p->x, p->y});
            }
        }
        return 0;
    }
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        on_mouse_button(proto::MouseButton::Right, msg == WM_RBUTTONDOWN, lp);
        return 0;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        on_mouse_button(proto::MouseButton::Middle, msg == WM_MBUTTONDOWN, lp);
        return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        on_mouse_button(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? proto::MouseButton::X1 : proto::MouseButton::X2,
                        msg == WM_XBUTTONDOWN, lp);
        return TRUE;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};  // screen coordinates for wheel messages
        ScreenToClient(hwnd, &pt);
        if (input_active() && to_normalized(pt)) {
            input_.wheel(GET_WHEEL_DELTA_WPARAM(wp), msg == WM_MOUSEHWHEEL);
            return 0;
        }
        break;
    }
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP: {
        // Alt+F4 stays local (closes the viewer) so it can never close the window on A by accident.
        const bool alt_f4 = wp == VK_F4 && (lp & (1 << 29)) != 0;
        if (input_active() && !alt_f4) {
            input_.key(wp, lp, msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
            return 0;  // consumed: Alt must not open the system menu here
        }
        break;
    }
    case WM_KILLFOCUS:
        input_.release_all();  // nothing stays pressed on A when the viewer loses focus
        break;
    case WM_MOUSELEAVE:
        tracking_mouse_ = false;
        if (hover_ != viewer::StripButton::None) {
            hover_ = viewer::StripButton::None;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (hover_ != viewer::StripButton::None) {
            pressed_ = hover_;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else {
            on_mouse_button(proto::MouseButton::Left, true, lp);
        }
        return 0;
    case WM_LBUTTONUP:
        if (pressed_ == viewer::StripButton::None) {
            on_mouse_button(proto::MouseButton::Left, false, lp);
        } else {
            RECT rc;
            GetClientRect(hwnd, &rc);
            const auto released_on = viewer::hit_test(viewer::layout_strip(static_cast<float>(rc.right), dpi_, text_),
                                                      static_cast<float>(GET_X_LPARAM(lp)),
                                                      static_cast<float>(GET_Y_LPARAM(lp)));
            const auto was_pressed = std::exchange(pressed_, viewer::StripButton::None);
            ReleaseCapture();
            if (released_on == was_pressed) {
                on_strip_click(was_pressed);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kHudTimer);
        KillTimer(hwnd, kBlinkTimer);
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

LRESULT ViewerWindow::hit_test_nc(POINT screen) const {
    POINT pt = screen;
    ScreenToClient(hwnd_, &pt);
    RECT rc;
    GetClientRect(hwnd_, &rc);

    if (!IsZoomed(hwnd_)) {
        const int border = px(vt::resize_border, dpi_);
        const bool left = pt.x < border;
        const bool right = pt.x >= rc.right - border;
        const bool top = pt.y < border;
        const bool bottom = pt.y >= rc.bottom - border;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }
    const auto layout = viewer::layout_strip(static_cast<float>(rc.right), dpi_, text_);
    if (static_cast<float>(pt.y) < layout.strip.bottom) {
        const bool on_button = viewer::hit_test(layout, static_cast<float>(pt.x), static_cast<float>(pt.y)) !=
                               viewer::StripButton::None;
        return on_button ? HTCLIENT : HTCAPTION;
    }
    return HTCLIENT;
}

void ViewerWindow::on_strip_click(viewer::StripButton button) {
    switch (button) {
    case viewer::StripButton::Close:
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        break;
    case viewer::StripButton::Maximize:
        ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
        break;
    case viewer::StripButton::Minimize:
        ShowWindow(hwnd_, SW_MINIMIZE);
        break;
    case viewer::StripButton::Fit:
        one_to_one_ = !one_to_one_;
        break;
    case viewer::StripButton::Hud:
        hud_visible_ = !hud_visible_;
        break;
    case viewer::StripButton::None:
        break;
    }
}

void ViewerWindow::apply_dpi(UINT dpi) {
    dpi_ = dpi;
    text_.create(dpi);
}

void ViewerWindow::resize_for_stream(int width, int height) {
    if (IsZoomed(hwnd_) || width <= 0 || height <= 0) {
        return;
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    const int work_w = work.right - work.left;
    const int work_h = work.bottom - work.top;
    const int strip = px(vt::strip_h, dpi_);

    // Native size when it fits (pixel-exact), otherwise scaled down to 80% of the work area.
    const RECT limit{0, 0, static_cast<int>(std::lround(work_w * vt::stream_fit_fraction)),
                     static_cast<int>(std::lround(work_h * vt::stream_fit_fraction)) - strip};
    int client_w = width;
    int image_h = height;
    if (width > limit.right || height > limit.bottom) {
        const RECT fit = fit_rect(width, height, limit);
        client_w = fit.right - fit.left;
        image_h = fit.bottom - fit.top;
    }
    const int total_w = std::max(client_w, px(vt::min_client_w, dpi_));
    const int total_h = std::max(image_h + strip, px(vt::min_client_h, dpi_));
    SetWindowPos(hwnd_, nullptr, work.left + (work_w - total_w) / 2, work.top + (work_h - total_h) / 2, total_w,
                 total_h, SWP_NOZORDER | SWP_NOACTIVATE);
}

bool ViewerWindow::ensure_render_target() {
    if (rt_) {
        return true;
    }
    RECT rc;
    GetClientRect(hwnd_, &rc);
    // 96 dpi: one D2D unit is one physical pixel; everything is scaled with px().
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.0f, 96.0f);
    const auto hwnd_props = D2D1::HwndRenderTargetProperties(
        hwnd_, D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom)));
    if (FAILED(ui::Graphics::get().d2d()->CreateHwndRenderTarget(props, hwnd_props, &rt_))) {
        log::error("viewer: CreateHwndRenderTarget failed");
        return false;
    }
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);  // opaque target
    rt_->CreateSolidColorBrush(ui::to_color(pal().ink), &brush_);
    bitmap_.Reset();
    bitmap_dirty_ = true;
    return true;
}

void ViewerWindow::take_pending_frame() {
    FrameInfo info;
    bool got = false;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        generation = shared_.generation;
        if (shared_.has_pending) {
            std::swap(shared_.pending, displayed_);  // the old displayed buffer becomes the next spare
            info = shared_.pending_info;
            shared_.has_pending = false;
            got = true;
        }
    }
    if (generation != seen_generation_) {
        // New stream (or the old one ended): per-stream state starts over.
        input_.release_all();
        seen_generation_ = generation;
        have_frame_ = false;
        stream_sized_ = false;
        last_seq_.reset();
        dropped_ = 0;
        latency_ms_.reset();
    }
    if (!got) {
        return;
    }

    bitmap_dirty_ = true;
    have_frame_ = true;
    last_frame_time_ = Clock::now();
    ++frames_shown_;
    if (last_seq_ && info.seq > *last_seq_ + 1) {
        dropped_ += info.seq - *last_seq_ - 1;
    }
    last_seq_ = info.seq;
    if (info.t_capture_local_us) {
        latency_ms_ = static_cast<int>((steady_now_us() - *info.t_capture_local_us) / 1000);
    } else {
        latency_ms_.reset();
    }
    if (!stream_sized_) {
        stream_sized_ = true;
        resize_for_stream(displayed_.width, displayed_.height);
    }
}

ViewerWindow::View ViewerWindow::current_view() const {
    LinkState link;
    {
        std::lock_guard lock(mutex_);
        link = shared_.link;
    }
    switch (link) {
    case LinkState::Waiting:
        return View::Waiting;
    case LinkState::Connecting:
        return View::Connecting;
    case LinkState::Lost:
        return View::Lost;
    case LinkState::Linked:
        break;
    }
    if (!have_frame_) {
        return View::Linked;
    }
    return Clock::now() - last_frame_time_ > std::chrono::milliseconds(vt::stall_after_ms) ? View::Stalled
                                                                                          : View::Streaming;
}

bool ViewerWindow::blinking_visible(View view) const {
    return view == View::Connecting;  // the status dot blinks while connecting
}

void ViewerWindow::on_blink_tick() {
    blink_on_ = reduced_motion_ ? true : !blink_on_;
    if (blinking_visible(current_view())) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void ViewerWindow::on_hud_tick() {
    const std::uint64_t bytes = payload_bytes_.load();
    bucket_ = (bucket_ + 1) % frame_buckets_.size();
    frame_buckets_[bucket_] = frames_shown_ - frames_at_last_tick_;
    byte_buckets_[bucket_] = bytes - bytes_at_last_tick_;
    frames_at_last_tick_ = frames_shown_;
    bytes_at_last_tick_ = bytes;

    {
        std::lock_guard lock(mutex_);
        hud_.rtt_ms = shared_.rtt_ms;
        hud_.jpeg_quality = shared_.jpeg_quality;
    }
    const View view = current_view();
    // MBPS is real whenever the link is up (frames may arrive without being shown, e.g. a codec
    // we do not display); the buckets cover exactly the last second.
    if (view == View::Linked || view == View::Streaming || view == View::Stalled) {
        hud_.mbps = static_cast<double>(std::accumulate(byte_buckets_.begin(), byte_buckets_.end(), std::uint64_t{0})) *
                    8.0 / 1e6;
    } else {
        hud_.mbps.reset();
    }
    if (view == View::Streaming || view == View::Stalled) {
        hud_.fps = static_cast<int>(std::accumulate(frame_buckets_.begin(), frame_buckets_.end(), std::uint64_t{0}));
        hud_.latency_ms = latency_ms_;
        hud_.width = displayed_.width;
        hud_.height = displayed_.height;
        hud_.dropped = dropped_;
    } else {
        hud_.fps.reset();
        hud_.latency_ms.reset();
        hud_.width.reset();
        hud_.height.reset();
        hud_.dropped.reset();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);  // also turns Streaming into Stalled on time
}

// ---------------------------------------------------------------------------------------------
// Rendering

void ViewerWindow::render() {
    if (!ensure_render_target()) {
        return;
    }
    take_pending_frame();
    const View view = current_view();

    RECT rc;
    GetClientRect(hwnd_, &rc);
    const auto w = static_cast<float>(rc.right);
    const auto h = static_cast<float>(rc.bottom);

    std::wstring stream_title;
    {
        std::lock_guard lock(mutex_);
        stream_title = shared_.stream_title;
    }
    const bool streaming = view == View::Streaming || view == View::Stalled;
    const std::wstring title = streaming ? stream_title : std::wstring();

    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    ui::Painter p(rt_.Get(), brush_.Get(), text_);
    const ui::Palette& c = pal();
    p.fill({0, 0, w, h}, c.bg);

    const auto layout = viewer::layout_strip(w, dpi_, text_);
    const D2D1_RECT_F content{0, layout.strip.bottom, w, h};
    draw_content(p, view, content);
    if (hud_visible_) {
        viewer::draw_hud(p, viewer::hud_rect(w, content.top, dpi_), hud_, dpi_);
    }

    viewer::StripModel strip;
    switch (view) {
    case View::Waiting:
        strip.state_word = L"Waiting";
        strip.status_color = c.ink3;
        break;
    case View::Connecting:
        strip.state_word = L"Connecting";
        strip.status_color = c.warn;
        strip.status_visible = blink_on_;
        break;
    case View::Linked:
        strip.state_word = L"Linked";
        strip.status_color = c.ok;
        break;
    case View::Streaming:
        strip.state_word = L"Streaming";
        strip.status_color = c.accent_ink;
        break;
    case View::Stalled:
        strip.state_word = L"Stalled";
        strip.status_color = c.warn;
        break;
    case View::Lost:
        strip.state_word = L"Connection lost";
        strip.status_color = c.err;
        break;
    }
    strip.title = title;
    strip.hud_on = hud_visible_;
    strip.one_to_one = one_to_one_;
    strip.maximized = IsZoomed(hwnd_) != FALSE;
    strip.hover = hover_;
    strip.pressed = pressed_;
    viewer::draw_strip(p, layout, strip, dpi_);

    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) {
        rt_.Reset();
        brush_.Reset();
        bitmap_.Reset();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    const std::wstring window_text = title.empty() ? std::wstring(ui::kAppName) : title;
    if (window_text != window_text_) {
        window_text_ = window_text;
        SetWindowTextW(hwnd_, window_text_.c_str());  // taskbar / Alt+Tab
    }
}

void ViewerWindow::draw_content(ui::Painter& p, View view, const D2D1_RECT_F& content) {
    if (view != View::Streaming && view != View::Stalled) {
        image_rect_ = {};
        draw_state_block(p, view, content);
        return;
    }
    if (!displayed_.empty()) {
        const auto fw = static_cast<UINT32>(displayed_.width);
        const auto fh = static_cast<UINT32>(displayed_.height);
        if (bitmap_dirty_ || !bitmap_) {
            const auto size = bitmap_ ? bitmap_->GetPixelSize() : D2D1_SIZE_U{};
            if (!bitmap_ || size.width != fw || size.height != fh) {
                bitmap_.Reset();
                const auto props =
                    D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                rt_->CreateBitmap(D2D1::SizeU(fw, fh), nullptr, 0, props, &bitmap_);
            }
            if (bitmap_) {
                bitmap_->CopyFromMemory(nullptr, displayed_.pixels.data(), static_cast<UINT32>(displayed_.stride()));
            }
            bitmap_dirty_ = false;
        }

        D2D1_RECT_F dst;
        if (one_to_one_) {
            // 1:1 — centered and cropped, no scrolling.
            const float x = std::floor((content.left + content.right - static_cast<float>(fw)) / 2);
            const float y = std::floor((content.top + content.bottom - static_cast<float>(fh)) / 2);
            dst = {x, y, x + static_cast<float>(fw), y + static_cast<float>(fh)};
        } else {
            const RECT area{0, static_cast<LONG>(content.top), static_cast<LONG>(content.right),
                            static_cast<LONG>(content.bottom)};
            dst = to_rect_f(fit_rect(displayed_.width, displayed_.height, area));
        }
        image_rect_ = dst;
        const bool exact = dst.right - dst.left == static_cast<float>(fw) && dst.bottom - dst.top == static_cast<float>(fh);
        if (bitmap_) {
            rt_->PushAxisAlignedClip(content, D2D1_ANTIALIAS_MODE_ALIASED);
            rt_->DrawBitmap(bitmap_.Get(), dst, 1.0f,
                            exact ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            rt_->PopAxisAlignedClip();
        }
    }
    if (view == View::Stalled) {
        draw_stalled(p, content);
    }
}

void ViewerWindow::draw_state_block(ui::Painter& p, View view, const D2D1_RECT_F& content) {
    std::string host;
    std::uint16_t port = 0;
    bool reconnects = false;
    std::optional<int> rtt;
    {
        std::lock_guard lock(mutex_);
        host = shared_.host;
        port = shared_.port;
        reconnects = shared_.reconnects;
        rtt = shared_.rtt_ms;
    }
    const std::wstring endpoint = host.empty() ? std::wstring() : std::format(L"{}:{}", widen(host), port);

    // Centred like the website's hero: "Sarke", a big headline, a line or two of detail.
    std::wstring_view headline;
    std::vector<std::wstring> details;
    bool hint = false;
    switch (view) {
    case View::Waiting:
        headline = L"Waiting for the other PC";
        details.push_back(endpoint.empty() ? L"It can start any time." : L"Looking for " + endpoint);
        break;
    case View::Connecting:
        headline = L"Connecting";
        details.push_back(endpoint.empty() ? L"Linking up over your local network." : L"Connecting to " + endpoint);
        break;
    case View::Linked:
        headline = L"Linked";
        details.push_back(rtt ? std::format(L"Round trip {} ms", *rtt) : std::wstring(L"Round trip measuring\u2026"));
        hint = true;
        break;
    case View::Lost:
        headline = L"Connection lost";
        details.push_back(reconnects ? L"Reconnecting by itself." : L"Restart Sarke on this PC.");
        break;
    default:
        return;
    }

    const ui::Palette& c = pal();
    const auto& text = p.styles();
    const auto P = [this](float dp) { return static_cast<float>(px(dp, dpi_)); };
    p.halo(content);

    const float side = P(ui::space::s32);
    const float max_w = content.right - content.left - 2 * side;
    // The biggest headline that fits.
    const TextStyle big = text.measure(TextStyle::Display, headline) <= max_w ? TextStyle::Display : TextStyle::Headline;
    const float eyebrow_h = text.metrics(TextStyle::Title).line_px;
    const float big_h = text.metrics(big).line_px;
    const float body_h = text.metrics(TextStyle::Body).line_px;
    const float keycap_h = P(vt::keycap_h);
    const auto n = static_cast<float>(details.size());
    const float total = eyebrow_h + P(ui::space::s8) + big_h + P(ui::space::s16) + n * body_h +
                        (hint ? P(ui::space::s24) + keycap_h : 0);
    float y = std::floor(content.top + (content.bottom - content.top - total) / 2);
    const auto row = [&](float h) { return D2D1_RECT_F{content.left + side, y, content.right - side, y + h}; };

    p.text_center(TextStyle::Title, ui::kAppName, row(eyebrow_h), c.ink2);
    y += eyebrow_h + P(ui::space::s8);
    p.text_center(big, headline, row(big_h), c.ink);
    y += big_h + P(ui::space::s16);
    for (const auto& line : details) {
        p.text_center(TextStyle::Body, line, row(body_h), c.ink2);
        y += body_h;
    }

    if (hint) {
        // "On the other PC, press [Ctrl] [Alt] [Shift] [W] to send a window" with keycaps.
        y += P(ui::space::s24);
        constexpr std::wstring_view before = L"On the other PC, press";
        constexpr std::wstring_view after = L"to send a window";
        constexpr std::wstring_view keys[] = {L"Ctrl", L"Alt", L"Shift", L"W"};
        const float gap = P(ui::space::s8);
        const float pad = P(vt::keycap_pad);
        float width = std::round(text.measure(TextStyle::Body, before)) + std::round(text.measure(TextStyle::Body, after)) + 2 * gap;
        for (const auto k : keys) width += std::round(text.measure(TextStyle::Mono, k)) + 2 * pad + P(ui::space::s4);
        width -= P(ui::space::s4);
        if (width <= max_w) {
            float x = std::floor((content.left + content.right - width) / 2);
            const float body_top = std::floor(y + (keycap_h - body_h) / 2);
            x += std::round(p.text(TextStyle::Body, before, x, body_top, max_w, c.ink2)) + gap;
            for (const auto k : keys) {
                const float kw = std::round(text.measure(TextStyle::Mono, k)) + 2 * pad;
                const D2D1_RECT_F cap{x, y, x + kw, y + keycap_h};
                p.round(cap, P(ui::radius::xs), c.tile);
                p.text_center(TextStyle::Mono, k, cap, c.ink);
                x += kw + P(ui::space::s4);
            }
            x += gap - P(ui::space::s4);
            p.text(TextStyle::Body, after, x, body_top, max_w, c.ink2);
        } else {
            p.text_center(TextStyle::Body, L"On the other PC, press Ctrl+Alt+Shift+W", row(keycap_h), c.ink2);
        }
    }

    // This PC, small at the bottom (the website's meta line).
    const float caption_h = text.metrics(TextStyle::Caption).line_px;
    const float footer_top = content.bottom - P(vt::footer_bottom_gap) - caption_h;
    p.text_center(TextStyle::Caption, footer_, {content.left + side, footer_top, content.right - side, footer_top + caption_h},
                  c.ink3);
}

void ViewerWindow::draw_stalled(ui::Painter& p, const D2D1_RECT_F& content) {
    const ui::Palette& c = pal();
    p.fill(content, c.bg, ui::alpha::scrim);

    const double seconds = std::chrono::duration<double>(Clock::now() - last_frame_time_).count();
    const std::wstring detail = std::format(L"{:.1f} s without frames", seconds);
    constexpr std::wstring_view word = L"Stalled";

    // A tile in the middle, like the website's cards.
    const auto& text = p.styles();
    const auto P = [this](float dp) { return static_cast<float>(px(dp, dpi_)); };
    const float head_h = text.metrics(TextStyle::Headline).line_px;
    const float body_h = text.metrics(TextStyle::Body).line_px;
    const float pad = P(ui::space::s24);
    const float w = std::max(std::round(text.measure(TextStyle::Headline, word)),
                             std::round(text.measure(TextStyle::Body, L"999.9 s without frames"))) + 2 * pad;
    const float h = head_h + P(ui::space::s4) + body_h + 2 * pad;
    const float cx = std::floor((content.left + content.right - w) / 2);
    const float cy = std::floor((content.top + content.bottom - h) / 2);
    p.round({cx, cy, cx + w, cy + h}, P(ui::radius::md), c.tile);
    p.text_center(TextStyle::Headline, word, {cx, cy + pad, cx + w, cy + pad + head_h}, c.warn);
    const float by = cy + pad + head_h + P(ui::space::s4);
    p.text_center(TextStyle::Body, detail, {cx, by, cx + w, by + body_h}, c.ink2);
}

}  // namespace kvm
