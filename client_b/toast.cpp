#include "toast.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

#include "kvm/log.h"

#pragma comment(lib, "dwmapi.lib")

namespace kvm {

namespace {

namespace color = ui::color;
using ui::px;
using ui::TextStyle;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr wchar_t kClassName[] = L"SarkeToast";
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT kTickMs = 15;
constexpr milliseconds kInTime{220};
constexpr milliseconds kOutTime{180};

// Card (dp), docs/DESIGN.md 9.3.
constexpr float kWidth = 380;
constexpr float kMargin = 16;   // from the top and right of the work area
constexpr float kSlide = 32;    // how far it slides in (short, so it never shows on a monitor to the right)
constexpr float kPad = 16;
constexpr float kBar = 4;       // brand bar on the left
constexpr float kLogo = 40;
constexpr float kLogoGap = 14;
constexpr float kRowGap = 4;
constexpr float kClose = 24;
constexpr float kCloseInset = 8;
constexpr float kCloseGlyph = 8;
constexpr float kSquare = 8;
constexpr float kProgress = 2;

bool reduced_motion() {
    BOOL animate = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
    return !animate;
}

std::uint32_t kind_color(Toast::Kind kind) {
    switch (kind) {
    case Toast::Kind::Connected: return color::ok;
    case Toast::Kind::Lost: return color::err;
    default: return color::accent;
    }
}

bool register_class(WNDPROC wnd_proc) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    if (GetClassInfoExW(GetModuleHandleW(nullptr), kClassName, &wc)) return true;
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = kClassName;
    return RegisterClassExW(&wc) != 0;
}

double ease_out(double t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
double ease_in(double t) { return t * t; }

}  // namespace

Toast::~Toast() {
    if (hwnd_) DestroyWindow(hwnd_);
}

bool Toast::ensure_window() {
    if (hwnd_) return true;
    try {
        ui::Graphics::get();
    } catch (const std::exception& e) {
        log::error("toast: {}", e.what());
        return false;
    }
    if (!register_class(&Toast::wnd_proc)) return false;
    CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClassName, ui::kAppName,
                    WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;  // set in WM_NCCREATE
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    SetLayeredWindowAttributes(hwnd_, 0, 0, LWA_ALPHA);
    return true;
}

Toast::Geometry Toast::geometry() const {
    Geometry g;
    const HMONITOR monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);  // the main screen
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor, &mi);
    g.work = mi.rcWork;
    UINT dpi_x = 96, dpi_y = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) dpi_x = GetDpiForSystem();
    g.dpi = dpi_x;
    const auto P = [&](float dp) { return px(dp, g.dpi); };
    const auto spec_line = [&](TextStyle s) { return P(ui::text_style_spec(s).line_height_dp); };
    g.w = P(kWidth);
    const int text_h = spec_line(TextStyle::Label) + P(kRowGap) + spec_line(TextStyle::Title) +
                       (content_.text.empty() ? 0 : spec_line(TextStyle::Body));
    g.h = 2 * P(kPad) + std::max(P(kLogo), text_h);
    g.x_shown = g.work.right - P(kMargin) - g.w;
    g.x_hidden = g.x_shown + P(kSlide);
    g.y = g.work.top + P(kMargin);
    return g;
}

bool Toast::show(Content content, std::function<void()> on_click, std::chrono::milliseconds hold) {
    if (!ensure_window()) return false;
    content_ = std::move(content);
    on_click_ = std::move(on_click);
    hold_ = hold;
    reduced_motion_ = motion_override_ ? !*motion_override_ : reduced_motion();

    const Geometry g = geometry();
    if (g.dpi != geo_.dpi || !rt_) text_.create(g.dpi);
    geo_ = g;
    if (rt_) rt_->Resize(D2D1::SizeU(static_cast<UINT32>(g.w), static_cast<UINT32>(g.h)));

    const auto now = Clock::now();
    if (phase_ == Phase::Hold || phase_ == Phase::In) {
        // Already up: new content, full time again, no second slide.
        if (phase_ == Phase::Hold) {
            phase_start_ = now;
            held_ = milliseconds{0};
        }
        place(phase_ == Phase::Hold ? 0.0 : 1.0 - ease_out(std::min(1.0, (now - phase_start_) / std::chrono::duration<double>(kInTime))),
              1.0);
    } else {
        phase_ = Phase::In;
        phase_start_ = now;
        held_ = milliseconds{0};
        place(1.0, 0.0);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    SetTimer(hwnd_, kTickTimer, kTickMs, nullptr);
    return true;
}

void Toast::dismiss() {
    if (!hwnd_ || phase_ == Phase::Hidden || phase_ == Phase::Out) return;
    phase_ = Phase::Out;
    phase_start_ = Clock::now();
}

void Toast::place(double slide, double alpha) {
    if (reduced_motion_) slide = 0;  // animations off in Windows: fade only
    const int x = geo_.x_shown + static_cast<int>(std::lround((geo_.x_hidden - geo_.x_shown) * slide));
    SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(std::lround(std::clamp(alpha, 0.0, 1.0) * 255)), LWA_ALPHA);
    SetWindowPos(hwnd_, HWND_TOPMOST, x, geo_.y, geo_.w, geo_.h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void Toast::tick() {
    const auto now = Clock::now();
    const auto since = std::chrono::duration<double>(now - phase_start_);
    switch (phase_) {
    case Phase::In: {
        const double t = std::min(1.0, since / std::chrono::duration<double>(kInTime));
        place(1.0 - ease_out(t), ease_out(t));
        if (t >= 1.0) {
            phase_ = Phase::Hold;
            phase_start_ = now;
            held_ = milliseconds{0};
        }
        break;
    }
    case Phase::Hold: {
        const auto used = held_ + (hover_ ? milliseconds{0} : std::chrono::duration_cast<milliseconds>(now - phase_start_));
        if (used >= hold_) dismiss();
        InvalidateRect(hwnd_, nullptr, FALSE);  // the time line
        break;
    }
    case Phase::Out: {
        const double t = std::min(1.0, since / std::chrono::duration<double>(kOutTime));
        place(ease_in(t), 1.0 - ease_in(t));
        if (t >= 1.0) {
            phase_ = Phase::Hidden;
            KillTimer(hwnd_, kTickTimer);
            ShowWindow(hwnd_, SW_HIDE);
        }
        break;
    }
    case Phase::Hidden:
        KillTimer(hwnd_, kTickTimer);
        break;
    }
}

D2D1_RECT_F Toast::close_rect() const {
    const auto P = [this](float dp) { return static_cast<float>(px(dp, geo_.dpi)); };
    const float right = static_cast<float>(geo_.w) - P(kCloseInset);
    return {right - P(kClose), P(kCloseInset), right, P(kCloseInset) + P(kClose)};
}

LRESULT CALLBACK Toast::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<Toast*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;  // CreateWindowExW has not returned yet
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<Toast*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle_message(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Toast::handle_message(UINT msg, WPARAM wp, LPARAM lp) {
    const HWND hwnd = hwnd_;
    switch (msg) {
    case WM_PAINT:
        render();
        ValidateRect(hwnd, nullptr);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (rt_) rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        return 0;
    case WM_TIMER:
        if (wp == kTickTimer) tick();
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        if (!tracking_leave_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            tracking_leave_ = TrackMouseEvent(&tme) != FALSE;
        }
        if (!hover_) {
            hover_ = true;  // hold it while the pointer is on it
            if (phase_ == Phase::Hold) held_ += std::chrono::duration_cast<milliseconds>(Clock::now() - phase_start_);
        }
        const auto r = close_rect();
        const float x = static_cast<float>(GET_X_LPARAM(lp));
        const float y = static_cast<float>(GET_Y_LPARAM(lp));
        const bool on_close = x >= r.left && x < r.right && y >= r.top && y < r.bottom;
        if (on_close != close_hover_) {
            close_hover_ = on_close;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_leave_ = false;
        if (hover_) {
            hover_ = false;
            if (phase_ == Phase::Hold) phase_start_ = Clock::now();
        }
        close_hover_ = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && close_hover_) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP: {
        const bool on_close = close_hover_;
        dismiss();
        if (!on_close) {
            if (auto on_click = on_click_) on_click();  // a copy: the callback may show another toast
        }
        return 0;
    }
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        if (phase_ != Phase::Hidden) {
            const Geometry g = geometry();
            if (g.dpi != geo_.dpi) text_.create(g.dpi);
            geo_ = g;
            place(phase_ == Phase::Hold ? 0.0 : 1.0, 1.0);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
        rt_.Reset();
        brush_.Reset();
        phase_ = Phase::Hidden;
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Toast::render() {
    if (!rt_) {
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.0f, 96.0f);
        const auto hwnd_props = D2D1::HwndRenderTargetProperties(
            hwnd_, D2D1::SizeU(static_cast<UINT32>(geo_.w), static_cast<UINT32>(geo_.h)));
        if (FAILED(ui::Graphics::get().d2d()->CreateHwndRenderTarget(props, hwnd_props, &rt_))) return;
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        rt_->CreateSolidColorBrush(ui::to_color(color::text_hi), &brush_);
    }
    const auto P = [this](float dp) { return static_cast<float>(px(dp, geo_.dpi)); };
    const auto w = static_cast<float>(geo_.w);
    const auto h = static_cast<float>(geo_.h);
    const float line = static_cast<float>(ui::hairline(geo_.dpi));

    rt_->BeginDraw();
    rt_->Clear(ui::to_color(color::bg1));
    ui::Painter p(rt_.Get(), brush_.Get(), text_);
    p.frame({0, 0, w, h}, line, color::line_strong);
    p.fill({0, 0, P(kBar), h}, color::accent);

    // Logo tile.
    const float pad = P(kPad);
    const float logo = P(kLogo);
    const float lx = P(kBar) + pad;
    const float ly = std::round((h - logo) / 2);
    p.fill({lx, ly, lx + logo, ly + logo}, ui::logo::background);
    p.frame({lx, ly, lx + logo, ly + logo}, line, color::line);
    const float unit = logo / ui::logo::grid;
    for (const auto& c : ui::logo::cells) {
        p.fill({std::round(lx + c.x * unit), std::round(ly + c.y * unit), std::round(lx + (c.x + c.w) * unit),
                std::round(ly + (c.y + c.h) * unit)},
               c.rgb);
    }

    // Text block, vertically centred next to the logo.
    const float tx = lx + logo + P(kLogoGap);
    const auto close = close_rect();
    const float label_h = text_.metrics(TextStyle::Label).line_px;
    const float title_h = text_.metrics(TextStyle::Title).line_px;
    const float body_h = content_.text.empty() ? 0 : text_.metrics(TextStyle::Body).line_px;
    float ty = std::round((h - (label_h + P(kRowGap) + title_h + body_h)) / 2);
    const float sq = P(kSquare);
    p.fill({tx, std::round(ty + (label_h - sq) / 2), tx + sq, std::round(ty + (label_h - sq) / 2) + sq},
           kind_color(content_.kind));
    float x = tx + sq + P(ui::space::s8);
    x += p.text(TextStyle::Label, ui::kAppName, x, ty, close.left - x, color::text_hi);
    if (!content_.tag.empty()) {
        p.text(TextStyle::Label, std::wstring(ui::kSeparator) + content_.tag, x, ty, close.left - x, color::text_lo);
    }
    ty += label_h + P(kRowGap);
    p.text(TextStyle::Title, content_.title, tx, ty, w - pad - tx, color::text_hi);
    ty += title_h;
    if (!content_.text.empty()) p.text(TextStyle::Body, content_.text, tx, ty, w - pad - tx, color::text_mid);

    // Close.
    if (close_hover_) p.fill(close, color::bg2);
    const float g = P(kCloseGlyph);
    const float cx = std::round((close.left + close.right - g) / 2);
    const float cy = std::round((close.top + close.bottom - g) / 2);
    const auto ink = close_hover_ ? color::text_hi : color::text_lo;
    p.diagonal({cx, cy}, {cx + g, cy + g}, line, ink);
    p.diagonal({cx + g, cy}, {cx, cy + g}, line, ink);

    // Time left.
    if (phase_ == Phase::In || phase_ == Phase::Hold) {
        double left = 1.0;
        if (phase_ == Phase::Hold && hold_.count() > 0) {
            const auto used = held_ + (hover_ ? milliseconds{0}
                                              : std::chrono::duration_cast<milliseconds>(Clock::now() - phase_start_));
            left = std::clamp(1.0 - static_cast<double>(used.count()) / static_cast<double>(hold_.count()), 0.0, 1.0);
        }
        const float x0 = P(kBar);
        p.fill({x0, h - P(kProgress), x0 + std::round(static_cast<float>((w - x0) * left)), h}, color::accent,
               hover_ ? 0.35f : 0.8f);
    }

    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) {
        rt_.Reset();
        brush_.Reset();
    }
}

}  // namespace kvm
