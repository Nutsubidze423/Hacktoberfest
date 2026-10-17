#include "toast.h"

#include <windowsx.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

#include "kvm/log.h"

namespace kvm {

namespace {

using ui::pal;
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
constexpr float kMargin = 16;   // card edge from the top and right of the work area
constexpr float kShadow = 14;   // transparent room around the card for its shadow
constexpr float kSlide = 32;    // how far it slides in (short, so it never shows on a monitor to the right)
constexpr float kPad = 16;
constexpr float kLogo = 40;
constexpr float kLogoGap = 14;
constexpr float kRowGap = 2;
constexpr float kClose = 24;
constexpr float kCloseInset = 10;
constexpr float kCloseGlyph = 8;
constexpr float kDot = 7;
constexpr float kProgress = 2;
constexpr float kProgressInset = 8;
constexpr float kShadowAlpha = 0.045f;  // per shadow ring
constexpr int kShadowRings = 7;

bool reduced_motion() {
    BOOL animate = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
    return !animate;
}

std::uint32_t kind_color(Toast::Kind kind) {
    switch (kind) {
    case Toast::Kind::Connected: return pal().ok;
    case Toast::Kind::Lost: return pal().err;
    default: return pal().accent_ink;
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
    // Per-pixel alpha (UpdateLayeredWindow): the rounded corners and the shadow are really transparent.
    CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClassName, ui::kAppName,
                    WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    return hwnd_ != nullptr;  // set in WM_NCCREATE
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
    const auto line = [&](TextStyle s) { return P(ui::text_style_spec(s).line_height_dp); };
    g.margin = P(kShadow);
    const int text_h = line(TextStyle::Caption) + P(kRowGap) + line(TextStyle::Title) +
                       (content_.text.empty() ? 0 : line(TextStyle::Body));
    const int card_h = 2 * P(kPad) + std::max(P(kLogo), text_h) + P(kProgressInset);
    g.w = P(kWidth) + 2 * g.margin;
    g.h = card_h + 2 * g.margin;
    g.x_shown = g.work.right - P(kMargin) - P(kWidth) - g.margin;
    g.x_hidden = g.x_shown + P(kSlide);
    g.y = g.work.top + P(kMargin) - g.margin;
    return g;
}

bool Toast::show(Content content, std::function<void()> on_click, std::chrono::milliseconds hold) {
    if (!ensure_window()) return false;
    content_ = std::move(content);
    on_click_ = std::move(on_click);
    hold_ = hold;
    reduced_motion_ = motion_override_ ? !*motion_override_ : reduced_motion();

    const Geometry g = geometry();
    if (g.dpi != geo_.dpi || phase_ == Phase::Hidden) text_.create(g.dpi);
    geo_ = g;

    const auto now = Clock::now();
    if (phase_ == Phase::Hold || phase_ == Phase::In) {
        // Already up: new content, full time again, no second slide.
        if (phase_ == Phase::Hold) {
            phase_start_ = now;
            held_ = milliseconds{0};
        }
        render();
        place(phase_ == Phase::Hold ? 0.0 : 1.0 - ease_out(std::min(1.0, (now - phase_start_) / std::chrono::duration<double>(kInTime))),
              phase_ == Phase::Hold ? 1.0 : alpha_ / 255.0);
    } else {
        phase_ = Phase::In;
        phase_start_ = now;
        held_ = milliseconds{0};
        render();
        place(1.0, 0.0);
    }
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
    x_ = geo_.x_shown + static_cast<int>(std::lround((geo_.x_hidden - geo_.x_shown) * slide));
    alpha_ = static_cast<BYTE>(std::lround(std::clamp(alpha, 0.0, 1.0) * 255));
    canvas_.present(hwnd_, x_, geo_.y, alpha_);
    if (!IsWindowVisible(hwnd_)) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void Toast::tick() {
    const auto now = Clock::now();
    const auto since = std::chrono::duration<double>(now - phase_start_);
    switch (phase_) {
    case Phase::In: {
        const double t = std::min(1.0, since / std::chrono::duration<double>(kInTime));
        render();
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
        render();  // the time line
        place(0.0, 1.0);
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

D2D1_RECT_F Toast::card_rect() const {
    const auto m = static_cast<float>(geo_.margin);
    return {m, m, static_cast<float>(geo_.w) - m, static_cast<float>(geo_.h) - m};
}

D2D1_RECT_F Toast::close_rect() const {
    const auto P = [this](float dp) { return static_cast<float>(px(dp, geo_.dpi)); };
    const auto card = card_rect();
    const float right = card.right - P(kCloseInset);
    const float top = card.top + P(kCloseInset);
    return {right - P(kClose), top, right, top + P(kClose)};
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
            if (phase_ == Phase::Hold) {
                render();
                place(0.0, 1.0);
            }
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
    case WM_SETTINGCHANGE:
        if (msg == WM_SETTINGCHANGE && ui::is_theme_change(msg, lp)) ui::refresh_theme();
        if (phase_ != Phase::Hidden) {
            const Geometry g = geometry();
            if (g.dpi != geo_.dpi) text_.create(g.dpi);
            geo_ = g;
            render();
            place(phase_ == Phase::Hold ? 0.0 : 1.0, 1.0);
        }
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
        phase_ = Phase::Hidden;
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Toast::render() {
    ID2D1RenderTarget* rt = canvas_.begin(geo_.w, geo_.h);
    if (!rt) return;
    const ui::Palette& c = pal();
    const auto P = [this](float dp) { return static_cast<float>(px(dp, geo_.dpi)); };
    ui::Painter p(rt, canvas_.brush(), text_);
    const auto card = card_rect();
    const float k = P(ui::radius::md);

    // Soft shadow: rings of faint black, wider and fainter outwards (the card floats over any app).
    const float step = static_cast<float>(geo_.margin) / kShadowRings;
    for (int i = kShadowRings; i >= 1; --i) {
        const float o = step * static_cast<float>(i);
        p.round({card.left - o, card.top - o + step, card.right + o, card.bottom + o + step}, k + o, 0x000000,
                kShadowAlpha * static_cast<float>(kShadowRings - i + 1) / kShadowRings);
    }
    p.round(card, k, c.tile);
    p.round_stroke(card, k, static_cast<float>(ui::hairline(geo_.dpi)), c.ink, ui::alpha::line);

    // Logo.
    const float pad = P(kPad);
    const float logo = P(kLogo);
    const float lx = card.left + pad;
    const float ly = std::round(card.top + (card.bottom - card.top - P(kProgressInset) - logo) / 2);
    p.logo({lx, ly, lx + logo, ly + logo});

    // Text block next to the logo.
    const float tx = lx + logo + P(kLogoGap);
    const auto close = close_rect();
    const float caption_h = text_.metrics(TextStyle::Caption).line_px;
    const float title_h = text_.metrics(TextStyle::Title).line_px;
    const float body_h = content_.text.empty() ? 0 : text_.metrics(TextStyle::Body).line_px;
    float ty = std::round(card.top + (card.bottom - card.top - P(kProgressInset) - (caption_h + P(kRowGap) + title_h + body_h)) / 2);
    const float dot = P(kDot);
    p.circle({tx + dot / 2, ty + caption_h / 2}, dot / 2, kind_color(content_.kind));
    float x = tx + dot + P(6);
    x += p.text(TextStyle::Caption, ui::kAppName, x, ty, close.left - x, c.ink);
    if (!content_.tag.empty()) {
        p.text(TextStyle::Caption, std::wstring(ui::kSeparator) + content_.tag, x, ty, close.left - x, c.ink3);
    }
    ty += caption_h + P(kRowGap);
    p.text(TextStyle::Title, content_.title, tx, ty, card.right - pad - tx, c.ink);
    ty += title_h;
    if (!content_.text.empty()) p.text(TextStyle::Body, content_.text, tx, ty, card.right - pad - tx, c.ink2);

    // Close: a round button that shows on hover.
    if (close_hover_) p.circle({(close.left + close.right) / 2, (close.top + close.bottom) / 2}, P(kClose) / 2, c.fill_hi);
    const float g = P(kCloseGlyph);
    const float cx = std::round((close.left + close.right - g) / 2);
    const float cy = std::round((close.top + close.bottom - g) / 2);
    const auto ink = close_hover_ ? c.ink : c.ink3;
    const float line = std::max(1.0f, P(1.25f));
    p.diagonal({cx, cy}, {cx + g, cy + g}, line, ink);
    p.diagonal({cx + g, cy}, {cx, cy + g}, line, ink);

    // Time left: a thin pill near the bottom edge that runs out.
    if (phase_ == Phase::In || phase_ == Phase::Hold) {
        double left = 1.0;
        if (phase_ == Phase::Hold && hold_.count() > 0) {
            const auto used = held_ + (hover_ ? milliseconds{0}
                                              : std::chrono::duration_cast<milliseconds>(Clock::now() - phase_start_));
            left = std::clamp(1.0 - static_cast<double>(used.count()) / static_cast<double>(hold_.count()), 0.0, 1.0);
        }
        const float x0 = card.left + pad;
        const float x1 = card.right - pad;
        const float y1 = card.bottom - P(kProgressInset);
        p.pill({x0, y1 - P(kProgress), x1, y1}, c.ink, ui::alpha::line);
        const float run = std::round(static_cast<float>((x1 - x0) * left));
        if (run >= P(kProgress)) p.pill({x0, y1 - P(kProgress), x0 + run, y1}, c.accent_ink, hover_ ? 0.4f : 0.9f);
    }
    canvas_.end();
}

}  // namespace kvm
