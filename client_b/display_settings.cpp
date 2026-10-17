#include "display_settings.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <mutex>
#include <string_view>
#include <utility>

#include "kvm/log.h"

#pragma comment(lib, "dwmapi.lib")

namespace kvm {

namespace {

using display::Side;
using layout::Role;
using ui::pal;
using ui::px;
using ui::TextStyle;

constexpr wchar_t kClassName[] = L"SarkeDisplaySettings";
constexpr wchar_t kIdentifyClassName[] = L"SarkeIdentify";
constexpr wchar_t kWindowTitle[] = L"Sarke · Displays";
constexpr UINT kMsgPeer = WM_APP + 1;
constexpr UINT_PTR kRefreshTimer = 1;   // re-checks the monitors in case a change message was missed
constexpr UINT_PTR kIdentifyTimer = 2;
constexpr UINT kRefreshMs = 2000;
constexpr UINT kIdentifyMs = 2500;

// Window (dp), docs/DESIGN.md 10.
constexpr float kInitialW = 980;
constexpr float kInitialH = 700;
constexpr float kMinW = 820;
constexpr float kMinH = 640;
constexpr float kStripH = 44;
constexpr float kCaptionButtonW = 46;
constexpr float kCaptionGlyph = 10;
constexpr float kResizeBorder = 6;
constexpr float kLogo = 20;
constexpr float kPad = 40;            // page padding left / right
constexpr float kTilePad = 20;        // inside the map and panel tiles
constexpr float kPanelW = 300;
constexpr float kSegmentH = 32;
constexpr float kSegmentInset = 2;
constexpr float kButtonH = 36;
constexpr float kRowH = 30;
constexpr float kMapMargin = 44;      // around the drawn desktop, room for the Live line
constexpr float kEdgeBar = 3;         // the crossing edge
constexpr float kPeerStroke = 2;
constexpr float kDot = 7;
constexpr float kIdentifySize = 200;  // the popup on each monitor
constexpr float kIdentifyInset = 48;
constexpr float kIdentifyNumber = 96;

constexpr Side kSides[4] = {Side::Left, Side::Right, Side::Top, Side::Bottom};
constexpr DisplaySettings::Hit kSideHits[4] = {DisplaySettings::Hit::SideLeft, DisplaySettings::Hit::SideRight,
                                               DisplaySettings::Hit::SideTop, DisplaySettings::Hit::SideBottom};

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::wstring side_word(Side s) { return widen(display::to_string(s)); }

// "right" -> "Right" (sentence case, like the website).
std::wstring capitalized(std::wstring s) {
    if (!s.empty()) CharUpperBuffW(s.data(), 1);
    return s;
}

std::wstring percent(double scale) { return std::format(L"{}%", std::lround(scale * 100)); }

std::wstring size_text(int w, int h) { return std::format(L"{}{}{}", w, ui::kTimes, h); }

int monitor_number(const display::Monitor& m, std::size_t index) {
    const int n = layout::display_number(m.name);
    return n > 0 ? n : static_cast<int>(index) + 1;
}

float width(const D2D1_RECT_F& r) { return r.right - r.left; }
float height(const D2D1_RECT_F& r) { return r.bottom - r.top; }
bool contains(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

bool register_class(const wchar_t* name, WNDPROC proc) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    if (GetClassInfoExW(GetModuleHandleW(nullptr), name, &wc)) return true;
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = name;
    return RegisterClassExW(&wc) != 0;
}

bool same_monitors(const std::vector<display::Monitor>& a, const std::vector<display::Monitor>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const auto& x, const auto& y) {
        return x.rect == y.rect && x.dpi == y.dpi && x.primary == y.primary && x.name == y.name;
    });
}

}  // namespace

// ---- Identify: the monitor's number, big, in its bottom-left corner (like Windows' Identify) ----
// A rounded tile with real transparency (UpdateLayeredWindow), drawn once.

class IdentifyPopup {
public:
    IdentifyPopup(const display::Monitor& m, int number) {
        const UINT dpi = m.dpi ? m.dpi : 96;
        if (!register_class(kIdentifyClassName, &IdentifyPopup::wnd_proc)) return;
        const int size = px(kIdentifySize, dpi);
        const int inset = px(kIdentifyInset, dpi);
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kIdentifyClassName,
                                L"", WS_POPUP, 0, 0, size, size, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd_) return;
        try {
            draw(m, number, dpi, size);
        } catch (const std::exception& e) {
            log::error("displays: identify: {}", e.what());
            return;
        }
        canvas_.present(hwnd_, m.work.left + inset, m.work.bottom - inset - size, 255);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
    ~IdentifyPopup() {
        if (hwnd_) DestroyWindow(hwnd_);
    }
    IdentifyPopup(const IdentifyPopup&) = delete;
    IdentifyPopup& operator=(const IdentifyPopup&) = delete;

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
            ShowWindow(hwnd, SW_HIDE);  // click it away
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    void draw(const display::Monitor& m, int number, UINT dpi, int size) {
        ID2D1RenderTarget* rt = canvas_.begin(size, size);
        if (!rt) return;
        ui::TextStyles text;
        text.create(dpi);
        ui::Painter p(rt, canvas_.brush(), text);
        const ui::Palette& c = pal();
        const auto s = static_cast<float>(size);
        const auto P = [dpi](float dp) { return static_cast<float>(px(dp, dpi)); };
        p.round({0, 0, s, s}, P(ui::radius::lg), c.tile, 0.97f);
        const float pad = P(ui::space::s24);
        // The number, in the display face at Identify size.
        auto& g = ui::Graphics::get();
        ui::ComPtr<IDWriteTextFormat> big;
        const std::wstring n = std::to_wstring(number);
        if (SUCCEEDED(g.dwrite()->CreateTextFormat(g.family(ui::FontRole::Sans), g.fonts(ui::FontRole::Sans),
                                                   DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                                   DWRITE_FONT_STRETCH_NORMAL, P(kIdentifyNumber), L"en-us", &big))) {
            canvas_.brush()->SetColor(ui::to_color(c.ink));
            rt->DrawTextW(n.c_str(), static_cast<UINT32>(n.size()), big.Get(), {pad, pad / 2, s - pad, s - pad},
                          canvas_.brush());
        }
        const float line = text.metrics(TextStyle::Caption).line_px;
        const float logo = P(18);
        p.logo({pad, s - pad - 2 * line - P(6) - logo + line, pad + logo, s - pad - 2 * line - P(6) + line});
        p.text(TextStyle::BodyStrong, ui::kAppName, pad + logo + P(6), s - pad - 2 * line - P(4), s - 2 * pad, c.ink);
        p.text(TextStyle::Caption, std::format(L"{}{}{}", size_text(m.rect.width(), m.rect.height()), ui::kSeparator,
                                               percent(m.scale)),
               pad, s - pad - line, s - 2 * pad, c.ink2);
        canvas_.end();
    }

    HWND hwnd_ = nullptr;
    ui::LayeredSurface canvas_;
};

// ---- window ------------------------------------------------------------------------------------

DisplaySettings::DisplaySettings() = default;

DisplaySettings::~DisplaySettings() {
    on_close_ = nullptr;  // the owner is going away too
    identify_.clear();
    if (hwnd_) DestroyWindow(hwnd_);
}

bool DisplaySettings::show(Options options) {
    if (hwnd_) {
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        return true;
    }
    try {
        ui::Graphics::get();
    } catch (const std::exception& e) {
        log::error("displays: {}", e.what());
        return false;
    }
    if (!register_class(kClassName, &DisplaySettings::wnd_proc)) return false;

    options_ = std::move(options);
    computer_ = layout::computer_name();
    mons_ = layout::local_monitors();
    cfg_ = layout::read_config(options_.config_path);
    // The app that runs here knows best; else config.json's host_ip. The user can switch it.
    role_ = options_.role ? *options_.role : layout::running_role().value_or(layout::guess_role(cfg_.host_ip));
    if (!peer_set_) {
        if (auto p = layout::load_peer()) peer_ = *p;
    }
    reload_config();

    const UINT system_dpi = GetDpiForSystem();
    const HWND hwnd = CreateWindowExW(0, kClassName, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                      px(kInitialW, system_dpi), px(kInitialH, system_dpi), nullptr, nullptr,
                                      GetModuleHandleW(nullptr), this);
    if (!hwnd) return false;
    ui::style_window(hwnd);  // rounded corners, no system border, theme-matched shadow

    // Centre on the monitor with the cursor, at that monitor's DPI, inside its work area.
    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor, &mi);
    UINT mdpi_x = 96, mdpi_y = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &mdpi_x, &mdpi_y))) mdpi_x = system_dpi;
    apply_dpi(mdpi_x);
    const RECT& work = mi.rcWork;
    const int w = std::min(px(kInitialW, dpi_), static_cast<int>(work.right - work.left));
    const int h = std::min(px(kInitialH, dpi_), static_cast<int>(work.bottom - work.top));
    SetWindowPos(hwnd, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2,
                 w, h, SWP_NOZORDER | SWP_FRAMECHANGED);
    apply_dpi(GetDpiForWindow(hwnd));

    SetTimer(hwnd, kRefreshTimer, kRefreshMs, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    log::info("displays: {} monitor(s), config {}", mons_.size(), cfg_.path.string());
    return true;
}

void DisplaySettings::set_peer(const layout::Peer& peer, bool connected) {
    if (!peer.known() || peer.width <= 0 || peer.height <= 0) return;
    if (connected) layout::remember_peer(peer);
    HWND hwnd = nullptr;
    {
        std::lock_guard lock(peer_mutex_);
        incoming_peer_.emplace(peer, connected);
        hwnd = hwnd_;
    }
    if (hwnd) {
        PostMessageW(hwnd, kMsgPeer, 0, 0);
    } else {
        peer_ = peer;  // not open: picked up as is on show() (same thread rule as show())
        peer_live_ = connected;
        peer_set_ = true;
    }
}

LRESULT CALLBACK DisplaySettings::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<DisplaySettings*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        {
            std::lock_guard lock(self->peer_mutex_);
            self->hwnd_ = hwnd;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<DisplaySettings*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle_message(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT DisplaySettings::handle_message(UINT msg, WPARAM wp, LPARAM lp) {
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
        if (rt_) rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {px(kMinW, dpi_), px(kMinH, dpi_)};
        return 0;
    }
    case WM_DPICHANGED: {
        apply_dpi(HIWORD(wp));
        const auto* suggested = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        refresh_monitors();
        return 0;
    }
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        if (ui::is_theme_change(msg, lp)) {  // Windows switched between dark and light apps
            ui::refresh_theme();
            ui::style_window(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        refresh_monitors();
        break;
    case WM_TIMER:
        if (wp == kRefreshTimer) {
            refresh_monitors();
        } else if (wp == kIdentifyTimer) {
            close_identify();
        }
        return 0;
    case WM_ACTIVATE:
        // Back from Windows' display settings or a text editor: pick up what changed there.
        if (LOWORD(wp) != WA_INACTIVE) {
            if (!dirty() && status_ != Status::Error) reload_config();
            if (!peer_set_) {
                if (auto p = layout::load_peer()) peer_ = *p;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case kMsgPeer: {
        std::optional<std::pair<layout::Peer, bool>> p;
        {
            std::lock_guard lock(peer_mutex_);
            p.swap(incoming_peer_);
        }
        if (p) {
            peer_ = p->first;
            peer_live_ = p->second;
            peer_set_ = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (dragging_ || hover_ == Hit::Peer)) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!tracking_leave_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            tracking_leave_ = TrackMouseEvent(&tme) != FALSE;
        }
        if (dragging_) {
            if (!drag_moved_ && std::abs(pt.x - drag_start_.x) < GetSystemMetricsForDpi(SM_CXDRAG, dpi_) &&
                std::abs(pt.y - drag_start_.y) < GetSystemMetricsForDpi(SM_CYDRAG, dpi_)) {
                return 0;
            }
            drag_moved_ = true;
            const Layout l = compute_layout();
            const float w = width(drag_box_);
            const float h = height(drag_box_);
            // The box follows the pointer but its centre stays on the map.
            const float left = std::clamp(static_cast<float>(pt.x) - drag_grab_.x, l.map.left - w / 2, l.map.right - w / 2);
            const float top = std::clamp(static_cast<float>(pt.y) - drag_grab_.y, l.map.top - h / 2, l.map.bottom - h / 2);
            drag_box_ = {left, top, left + w, top + h};
            const auto desk = map_transform(l).to_client(display::virtual_desktop(mons_));
            const display::Rect around{static_cast<int>(desk.left), static_cast<int>(desk.top),
                                       static_cast<int>(desk.right), static_cast<int>(desk.bottom)};
            drag_side_ = layout::nearest_side(
                around, {static_cast<int>(left + w / 2), static_cast<int>(top + h / 2)}, layout::kVerticalSidesAccepted);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        Hit hit = hit_test(pt);
        if (!hit_enabled(hit)) hit = Hit::None;
        if (hit != hover_) {
            hover_ = hit;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_leave_ = false;
        if (hover_ != Hit::None) {
            hover_ = Hit::None;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const Hit hit = hit_test(pt);
        if (!hit_enabled(hit)) return 0;
        if (hit == Hit::Peer) {
            const Layout l = compute_layout();
            drag_box_ = peer_box(l, map_transform(l));
            drag_grab_ = {static_cast<float>(pt.x) - drag_box_.left, static_cast<float>(pt.y) - drag_box_.top};
            drag_start_ = pt;
            drag_side_ = pending_side_;
            drag_moved_ = false;
            dragging_ = true;
        } else {
            pressed_ = hit;
        }
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const bool was_dragging = dragging_;
        const bool moved = drag_moved_;
        const Hit pressed = pressed_;
        dragging_ = false;
        pressed_ = Hit::None;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (was_dragging) {
            if (moved) set_pending(drag_side_);
        } else if (pressed != Hit::None && hit_test(pt) == pressed) {
            activate(pressed);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (dragging_ || pressed_ != Hit::None) {  // capture taken away mid-drag: drop it, change nothing
            dragging_ = false;
            pressed_ = Hit::None;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_LEFT: set_pending(Side::Left); return 0;
        case VK_RIGHT: set_pending(Side::Right); return 0;
        case VK_UP: set_pending(Side::Top); return 0;
        case VK_DOWN: set_pending(Side::Bottom); return 0;
        case VK_RETURN:
            if (dirty()) apply();
            return 0;
        case VK_ESCAPE:
            if (dragging_) {
                dragging_ = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (dirty() && saved_valid_) {
                revert();
            } else {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        close_identify();
        KillTimer(hwnd, kRefreshTimer);
        break;
    case WM_NCDESTROY: {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        {
            std::lock_guard lock(peer_mutex_);
            hwnd_ = nullptr;
        }
        rt_.Reset();
        brush_.Reset();
        dragging_ = false;
        hover_ = pressed_ = Hit::None;
        tracking_leave_ = false;
        if (auto on_close = on_close_) on_close();
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT DisplaySettings::hit_test_nc(POINT screen) const {
    POINT pt = screen;
    ScreenToClient(hwnd_, &pt);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (!IsZoomed(hwnd_)) {
        const int border = px(kResizeBorder, dpi_);
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
    if (pt.y < px(kStripH, dpi_)) {
        const Hit hit = hit_test(pt);
        return hit == Hit::Minimize || hit == Hit::Close ? HTCLIENT : HTCAPTION;
    }
    return HTCLIENT;
}

void DisplaySettings::apply_dpi(UINT dpi) {
    dpi_ = dpi ? dpi : 96;
    text_.create(dpi_);
    if (hwnd_) {
        small_icon_ = ui::create_logo_icon(ui::small_icon_size(dpi_));
        big_icon_ = ui::create_logo_icon(ui::large_icon_size(dpi_));
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon_.get()));
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon_.get()));
    }
}

void DisplaySettings::refresh_monitors() {
    auto mons = layout::local_monitors();
    if (same_monitors(mons, mons_)) return;
    log::info("displays: monitors changed, {} now", mons.size());
    mons_ = std::move(mons);
    if (dragging_) {  // the boxes moved under the pointer
        dragging_ = false;
        if (GetCapture() == hwnd_) ReleaseCapture();
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void DisplaySettings::reload_config() {
    const bool keep_saved_note = status_ == Status::Saved;
    cfg_ = layout::read_config(options_.config_path);
    saved_valid_ = true;
    status_ = Status::Clean;
    status_text_.clear();

    // Missing client_side: the apps use "right".
    const auto client_side = cfg_.client_side.empty() ? std::optional(Side::Right) : display::parse_side(cfg_.client_side);
    if (!cfg_.error.empty()) {
        saved_valid_ = false;
        status_ = Status::Error;
        status_text_ = widen(cfg_.error);
        saved_side_ = layout::peer_side_from(role_, Side::Right);
    } else if (!client_side) {
        saved_valid_ = false;
        status_ = Status::Error;
        status_text_ = std::format(L"config.json says client_side \"{}\" - Sarke won't start with that. Pick a side and press Apply.",
                                   widen(cfg_.client_side));
        saved_side_ = layout::peer_side_from(role_, Side::Right);
    } else {
        saved_side_ = layout::peer_side_from(role_, *client_side);
        if (!layout::side_accepted(*client_side)) {
            saved_valid_ = false;
            status_ = Status::Error;
            status_text_ = std::format(L"config.json says client_side \"{}\" - Sarke takes only left or right for now. Pick a side and press Apply.",
                                       widen(cfg_.client_side));
        }
    }
    pending_side_ = layout::side_accepted(saved_side_) ? saved_side_ : layout::peer_side_from(role_, Side::Right);
    if (keep_saved_note && status_ == Status::Clean) status_ = Status::Saved;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void DisplaySettings::set_role(Role role) {
    if (options_.role || role == role_) return;
    role_ = role;
    status_ = Status::Clean;
    reload_config();
}

void DisplaySettings::set_pending(Side side) {
    if (!layout::side_accepted(side) || mons_.empty()) return;
    pending_side_ = side;
    if (status_ != Status::Error || saved_valid_) {
        status_ = dirty() ? Status::Dirty : Status::Clean;
        status_text_.clear();
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void DisplaySettings::apply() {
    const Side client_side = layout::client_side_for(role_, pending_side_);
    if (const auto error = layout::save_client_side(cfg_.path, client_side); !error.empty()) {
        status_ = Status::Error;
        status_text_ = widen(error);
        log::error("displays: {}", error);
    } else {
        log::info("displays: client_side \"{}\" saved to {}", display::to_string(client_side), cfg_.path.string());
        status_ = Status::Clean;
        reload_config();
        status_ = Status::Saved;
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void DisplaySettings::revert() {
    status_ = Status::Clean;
    reload_config();
}

void DisplaySettings::identify() {
    close_identify();
    for (std::size_t i = 0; i < mons_.size(); ++i) {
        identify_.push_back(std::make_unique<IdentifyPopup>(mons_[i], monitor_number(mons_[i], i)));
    }
    if (hwnd_) SetTimer(hwnd_, kIdentifyTimer, kIdentifyMs, nullptr);
}

void DisplaySettings::close_identify() {
    if (hwnd_) KillTimer(hwnd_, kIdentifyTimer);
    identify_.clear();
}

void DisplaySettings::activate(Hit hit) {
    switch (hit) {
    case Hit::Minimize: ShowWindow(hwnd_, SW_MINIMIZE); break;
    case Hit::Close: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    case Hit::SideLeft: set_pending(Side::Left); break;
    case Hit::SideRight: set_pending(Side::Right); break;
    case Hit::SideTop: set_pending(Side::Top); break;
    case Hit::SideBottom: set_pending(Side::Bottom); break;
    case Hit::RoleHost: set_role(Role::Host); break;
    case Hit::RoleClient: set_role(Role::Client); break;
    case Hit::Apply: apply(); break;
    case Hit::Revert: revert(); break;
    case Hit::Identify: identify(); break;
    case Hit::WindowsSettings:
        ShellExecuteW(hwnd_, L"open", L"ms-settings:display", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    default: break;
    }
}

bool DisplaySettings::dirty() const { return !saved_valid_ || pending_side_ != saved_side_; }

std::wstring DisplaySettings::peer_name() const { return peer_.known() ? widen(peer_.hostname) : L"the other PC"; }

// ---- layout ------------------------------------------------------------------------------------

D2D1_RECT_F DisplaySettings::MapTransform::to_client(const display::Rect& r) const {
    return {static_cast<float>(std::round(r.left * scale + dx)), static_cast<float>(std::round(r.top * scale + dy)),
            static_cast<float>(std::round(r.right * scale + dx)), static_cast<float>(std::round(r.bottom * scale + dy))};
}

DisplaySettings::Layout DisplaySettings::compute_layout() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const auto w = static_cast<float>(rc.right);
    const auto h = static_cast<float>(rc.bottom);
    const auto P = [this](float dp) { return static_cast<float>(px(dp, dpi_)); };
    const auto caption_h = text_.metrics(TextStyle::Caption).line_px;
    const auto body_h = text_.metrics(TextStyle::Body).line_px;

    Layout l;
    l.strip = {0, 0, w, P(kStripH)};
    l.close = {w - P(kCaptionButtonW), 0, w, l.strip.bottom};
    l.minimize = {l.close.left - P(kCaptionButtonW), 0, l.close.left, l.strip.bottom};
    l.x0 = P(kPad);
    l.x1 = w - P(kPad);

    float y = l.strip.bottom + P(ui::space::s16);
    l.label_top = y;
    y += caption_h + P(ui::space::s4);
    l.title_top = y;
    y += text_.metrics(TextStyle::Headline).line_px + P(ui::space::s8);
    l.body_top = y;
    y += 2 * body_h + P(ui::space::s24);
    const float main_top = y;

    const float footer_bottom = h - P(ui::space::s24);
    l.footer = {l.x0, footer_bottom - P(kButtonH), l.x1, footer_bottom};
    l.status_top = l.footer.top - P(ui::space::s16) - body_h;
    const float main_bottom = std::max(main_top + P(ui::space::s48 * 3), l.status_top - P(ui::space::s16));
    l.panel = {l.x1 - P(kPanelW), main_top, l.x1, main_bottom};
    l.map = {l.x0, main_top, l.panel.left - P(ui::space::s16), main_bottom};

    const float pl = l.panel.left + P(kTilePad);
    const float pr = l.panel.right - P(kTilePad);
    float py = l.panel.top + P(kTilePad);
    l.side_label_top = py;
    py += caption_h + P(ui::space::s8);
    // Segmented control: a pill track with four equal segments inside it.
    const float inset = P(kSegmentInset);
    const float seg_w = std::floor((pr - pl - 2 * inset) / 4);
    for (int i = 0; i < 4; ++i) {
        const float left = pl + inset + static_cast<float>(i) * seg_w;
        l.sides[i] = {left, py + inset, i == 3 ? pr - inset : left + seg_w, py + P(kSegmentH) - inset};
    }
    py += P(kSegmentH) + P(ui::space::s8);
    l.side_note_top = py;
    py += caption_h + P(ui::space::s16);
    l.role_label_top = py;
    py += caption_h + P(ui::space::s8);
    const float half = std::floor((pr - pl - 2 * inset) / 2);
    l.roles[0] = {pl + inset, py + inset, pl + inset + half, py + P(kSegmentH) - inset};
    l.roles[1] = {pl + inset + half, py + inset, pr - inset, py + P(kSegmentH) - inset};
    py += P(kSegmentH) + P(ui::space::s16);
    l.rows_top = py;
    const float bt = l.panel.bottom - P(kTilePad) - P(kButtonH);
    const float bhalf = std::floor((pr - pl - P(ui::space::s8)) / 2);
    l.apply = {pl, bt, pl + bhalf, bt + P(kButtonH)};
    l.revert = {pl + bhalf + P(ui::space::s8), bt, pr, bt + P(kButtonH)};

    // Footer: "Windows display settings ›" is a link (accent text), Identify a grey pill.
    const float pad2 = 2 * P(ui::space::s16);
    const float ws_w = std::round(text_.measure(TextStyle::BodyStrong, L"Windows display settings \u203A")) + P(ui::space::s8);
    l.windows_settings = {l.x1 - ws_w, l.footer.top, l.x1, l.footer.bottom};
    const float id_w = std::round(text_.measure(TextStyle::BodyStrong, L"Identify")) + pad2;
    l.identify = {l.windows_settings.left - P(ui::space::s16) - id_w, l.footer.top,
                  l.windows_settings.left - P(ui::space::s16), l.footer.bottom};
    return l;
}

DisplaySettings::MapTransform DisplaySettings::map_transform(const Layout& l) const {
    MapTransform t;
    if (mons_.empty()) return t;
    // This PC's desktop plus the other PC on every side it may go to: the scale stays put while
    // dragging and after a change.
    display::Rect u = display::virtual_desktop(mons_);
    for (Side s : kSides) {
        if (!layout::side_accepted(s) && s != pending_side_ && s != saved_side_) continue;
        const auto r = layout::peer_rect(mons_, s, peer_w(), peer_h());
        u = {std::min(u.left, r.left), std::min(u.top, r.top), std::max(u.right, r.right), std::max(u.bottom, r.bottom)};
    }
    const double margin = px(kMapMargin, dpi_);
    const double aw = std::max(1.0, width(l.map) - 2 * margin);
    const double ah = std::max(1.0, height(l.map) - 2 * margin);
    t.scale = std::min(aw / std::max(1, u.width()), ah / std::max(1, u.height()));
    t.dx = std::round(l.map.left + (width(l.map) - u.width() * t.scale) / 2 - u.left * t.scale);
    t.dy = std::round(l.map.top + (height(l.map) - u.height() * t.scale) / 2 - u.top * t.scale);
    return t;
}

D2D1_RECT_F DisplaySettings::peer_box(const Layout&, const MapTransform& t) const {
    if (dragging_ && drag_moved_) return drag_box_;
    return t.to_client(layout::peer_rect(mons_, pending_side_, peer_w(), peer_h()));
}

DisplaySettings::Hit DisplaySettings::hit_test(POINT client) const {
    const Layout l = compute_layout();
    const auto x = static_cast<float>(client.x);
    const auto y = static_cast<float>(client.y);
    if (contains(l.close, x, y)) return Hit::Close;
    if (contains(l.minimize, x, y)) return Hit::Minimize;
    for (int i = 0; i < 4; ++i) {
        if (contains(l.sides[i], x, y)) return kSideHits[i];
    }
    if (contains(l.roles[0], x, y)) return Hit::RoleHost;
    if (contains(l.roles[1], x, y)) return Hit::RoleClient;
    if (contains(l.apply, x, y)) return Hit::Apply;
    if (contains(l.revert, x, y)) return Hit::Revert;
    if (contains(l.identify, x, y)) return Hit::Identify;
    if (contains(l.windows_settings, x, y)) return Hit::WindowsSettings;
    if (contains(l.map, x, y) && !mons_.empty() && contains(peer_box(l, map_transform(l)), x, y)) return Hit::Peer;
    return Hit::None;
}

bool DisplaySettings::hit_enabled(Hit hit) const {
    switch (hit) {
    case Hit::None: return false;
    case Hit::SideTop:
    case Hit::SideBottom: return layout::kVerticalSidesAccepted && !mons_.empty();
    case Hit::SideLeft:
    case Hit::SideRight:
    case Hit::Peer: return !mons_.empty();
    case Hit::RoleHost:
    case Hit::RoleClient: return !options_.role.has_value();
    case Hit::Apply: return dirty() && !mons_.empty();
    case Hit::Revert: return dirty() && saved_valid_;
    case Hit::Identify: return !mons_.empty();
    default: return true;
    }
}

// ---- drawing -----------------------------------------------------------------------------------

bool DisplaySettings::ensure_render_target() {
    if (rt_) return true;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    // 96 dpi: one D2D unit is one physical pixel; everything is scaled with px().
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.0f, 96.0f);
    const auto hwnd_props = D2D1::HwndRenderTargetProperties(
        hwnd_, D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom)));
    if (FAILED(ui::Graphics::get().d2d()->CreateHwndRenderTarget(props, hwnd_props, &rt_))) {
        log::error("displays: CreateHwndRenderTarget failed");
        return false;
    }
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    rt_->CreateSolidColorBrush(ui::to_color(pal().ink), &brush_);
    return true;
}

void DisplaySettings::render() {
    if (!ensure_render_target()) return;
    const Layout l = compute_layout();
    rt_->BeginDraw();
    rt_->Clear(ui::to_color(pal().bg));
    ui::Painter p(rt_.Get(), brush_.Get(), text_);
    p.halo({0, 0, l.strip.right, static_cast<float>(rt_->GetPixelSize().height)});
    draw_strip(p, l);
    draw_header(p, l);
    draw_map(p, l);
    draw_panel(p, l);
    draw_footer(p, l);
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) {
        rt_.Reset();
        brush_.Reset();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void DisplaySettings::draw_strip(ui::Painter& p, const Layout& l) {
    const ui::Palette& c = pal();
    const auto P = [this](float dp) { return static_cast<float>(px(dp, dpi_)); };
    const float logo = P(kLogo);
    const float lx = P(14);
    const float ly = std::round((height(l.strip) - logo) / 2);
    p.logo({lx, ly, lx + logo, ly + logo});
    const float ty = std::round((height(l.strip) - text_.metrics(TextStyle::BodyStrong).line_px) / 2);
    float x = lx + logo + P(ui::space::s8);
    x += std::round(p.text(TextStyle::BodyStrong, ui::kAppName, x, ty, l.minimize.left - x, c.ink)) + P(ui::space::s8);
    p.text(TextStyle::Body, std::wstring(ui::kChevron) + L"  Displays", x, ty, l.minimize.left - x, c.ink2);

    const float glyph = P(kCaptionGlyph);
    const float line = static_cast<float>(ui::hairline(dpi_));
    const bool min_hot = hover_ == Hit::Minimize || pressed_ == Hit::Minimize;
    const bool close_hot = hover_ == Hit::Close || pressed_ == Hit::Close;
    if (min_hot) p.fill(l.minimize, c.tile);
    if (close_hot) p.fill(l.close, ui::kCloseHover);
    const float mcx = std::round((l.minimize.left + l.minimize.right - glyph) / 2);
    const float mcy = std::round((l.minimize.top + l.minimize.bottom) / 2);
    p.fill({mcx, mcy, mcx + glyph, mcy + line}, min_hot ? c.ink : c.ink2);
    const float cx = std::round((l.close.left + l.close.right - glyph) / 2);
    const float cy = std::round((l.close.top + l.close.bottom - glyph) / 2);
    const auto close_rgb = close_hot ? ui::kCloseGlyph : c.ink2;
    p.diagonal({cx, cy}, {cx + glyph, cy + glyph}, line, close_rgb);
    p.diagonal({cx + glyph, cy}, {cx, cy + glyph}, line, close_rgb);
}

void DisplaySettings::draw_header(ui::Painter& p, const Layout& l) {
    const ui::Palette& c = pal();
    const float w = l.x1 - l.x0;
    p.text(TextStyle::Caption, L"Settings \u00B7 Displays", l.x0, l.label_top, w, c.ink3);
    p.text(TextStyle::Headline, L"Where is the other PC?", l.x0, l.title_top, w, c.ink);
    const float body_h = text_.metrics(TextStyle::Body).line_px;
    p.text(TextStyle::Body, L"Drag the other PC to the side of your screens where it really stands.", l.x0, l.body_top,
           w, c.ink2);
    p.text(TextStyle::Body, L"The blue edge is where your mouse crosses over. Arrow keys work too.", l.x0,
           l.body_top + body_h, w, c.ink2);
}

void DisplaySettings::draw_map(ui::Painter& p, const Layout& l) {
    const ui::Palette& c = pal();
    const auto P = [this](float dp) { return static_cast<float>(px(dp, dpi_)); };
    const D2D1_RECT_F& map = l.map;
    p.round(map, P(ui::radius::lg), c.tile);

    const float caption_h = text_.metrics(TextStyle::Caption).line_px;
    const float pad = P(kTilePad);
    if (mons_.empty()) {
        p.text_center(TextStyle::Body, L"No monitors found", map, c.ink3);
        return;
    }
    rt_->PushAxisAlignedClip(map, D2D1_ANTIALIAS_MODE_ALIASED);
    const MapTransform t = map_transform(l);
    const bool dragging = dragging_ && drag_moved_;
    const Side side = dragging ? drag_side_ : pending_side_;
    const float stroke = std::max(1.0f, P(1.5f));

    // Where the other PC can go, while dragging.
    if (dragging) {
        for (Side s : kSides) {
            if (!layout::side_accepted(s)) continue;
            const auto zone = t.to_client(layout::peer_rect(mons_, s, peer_w(), peer_h()));
            if (s == drag_side_) {
                p.round(zone, P(ui::radius::xs), c.accent, 0.16f);
                p.round_stroke(zone, P(ui::radius::xs), stroke, c.accent_ink);
            } else {
                p.round_stroke(zone, P(ui::radius::xs), stroke, c.ink, ui::alpha::line_strong);
            }
        }
    }

    // This PC's monitors: rounded fills, number + size + scale.
    const float gap = std::max(1.0f, P(2));
    for (std::size_t i = 0; i < mons_.size(); ++i) {
        const auto& m = mons_[i];
        auto r = t.to_client(m.rect);
        r = {r.left + gap, r.top + gap, r.right - gap, r.bottom - gap};
        const float k = std::min(P(ui::radius::xs) * 1.5f, std::min(width(r), height(r)) / 6);
        p.round(r, k, c.fill);
        const float tx = r.left + P(ui::space::s12);
        const float tw = width(r) - 2 * P(ui::space::s12);
        float ty = r.top + P(ui::space::s8);
        const float title_h = text_.metrics(TextStyle::Title).line_px;
        const auto fits = [&](float h) { return ty + h <= r.bottom - P(ui::space::s4); };
        if (fits(title_h)) {
            p.text(TextStyle::Title, std::to_wstring(monitor_number(m, i)), tx, ty, tw, c.ink);
            ty += title_h + P(2);
        }
        if (fits(caption_h)) {
            p.text(TextStyle::Caption, size_text(m.rect.width(), m.rect.height()), tx, ty, tw, c.ink2);
            ty += caption_h;
        }
        if (fits(caption_h)) {
            p.text(TextStyle::Caption, m.primary ? percent(m.scale) + ui::kSeparator + L"Main" : percent(m.scale), tx, ty,
                   tw, c.ink3);
        }
    }

    // The crossing edge: a rounded blue bar on this PC's side of each piece.
    const float bar = P(kEdgeBar);
    for (const auto& piece : layout::outer_edge_pieces(mons_, side)) {
        const auto e = t.to_client(piece);
        D2D1_RECT_F r{};
        switch (side) {
        case Side::Left: r = {e.left + gap, e.top + gap, e.left + gap + bar, e.bottom - gap}; break;
        case Side::Right: r = {e.right - gap - bar, e.top + gap, e.right - gap, e.bottom - gap}; break;
        case Side::Top: r = {e.left + gap, e.top + gap, e.right - gap, e.top + gap + bar}; break;
        case Side::Bottom: r = {e.left + gap, e.bottom - gap - bar, e.right - gap, e.bottom - gap}; break;
        }
        if (width(r) > 0 && height(r) > 0) p.pill(r, c.accent_ink);
    }

    // The other PC.
    auto box = peer_box(l, t);
    box = {box.left + gap, box.top + gap, box.right - gap, box.bottom - gap};
    const bool hot = dragging_ || hover_ == Hit::Peer;
    const float k = std::min(P(ui::radius::xs) * 1.5f, std::min(width(box), height(box)) / 6);
    p.round(box, k, hot ? c.fill_hi : c.fill, dragging ? 0.94f : 1.0f);
    p.round_stroke(box, k, P(kPeerStroke), c.accent_ink);
    {
        const float tx = box.left + P(ui::space::s12);
        const float tw = width(box) - 2 * P(ui::space::s12);
        float ty = box.top + P(ui::space::s8);
        const float strong_h = text_.metrics(TextStyle::BodyStrong).line_px;
        const auto fits = [&](float h) { return ty + h <= box.bottom - P(ui::space::s4); };
        if (fits(caption_h)) {
            p.text(TextStyle::Caption, L"Other PC", tx, ty, tw, c.accent_ink);
            ty += caption_h + P(2);
        }
        if (fits(strong_h)) {
            p.text(TextStyle::BodyStrong, peer_.known() ? widen(peer_.hostname) : std::wstring(ui::kUnknown), tx, ty, tw,
                   c.ink);
            ty += strong_h;
        }
        if (fits(caption_h)) {
            p.text(TextStyle::Caption,
                   peer_.known() ? size_text(peer_w(), peer_h()) + ui::kSeparator + percent(peer_.scale)
                                 : std::wstring(L"Size not known yet"),
                   tx, ty, tw, c.ink2);
            ty += caption_h;
        }
        if (fits(caption_h)) {
            const bool live = peer_live_ && peer_.known();
            p.text(TextStyle::Caption, live ? L"Connected" : peer_.known() ? L"Last seen" : L"Not connected yet", tx, ty,
                   tw, live ? c.ok : c.ink3);
        }
    }

    // Live detection line.
    const display::Rect desk = display::virtual_desktop(mons_);
    const float dot = P(kDot);
    const float ly = map.top + pad;
    p.circle({map.left + pad + dot / 2, ly + caption_h / 2}, dot / 2, c.ok);
    p.text(TextStyle::Caption,
           std::format(L"Live{}{} monitor{}{}{}", ui::kSeparator, mons_.size(), mons_.size() == 1 ? L"" : L"s",
                       ui::kSeparator, size_text(desk.width(), desk.height())),
           map.left + pad + dot + P(ui::space::s8), ly, width(map) - 3 * pad - dot, c.ink3);
    rt_->PopAxisAlignedClip();
}

void DisplaySettings::draw_button(ui::Painter& p, const D2D1_RECT_F& r, std::wstring_view text, Hit hit, bool primary,
                                  bool selected) {
    // Segments (sides, roles): `selected` = the raised segment. Buttons: primary = blue pill,
    // otherwise a grey pill. Disabled = faded.
    const ui::Palette& c = pal();
    const bool segment = hit == Hit::SideLeft || hit == Hit::SideRight || hit == Hit::SideTop || hit == Hit::SideBottom ||
                         hit == Hit::RoleHost || hit == Hit::RoleClient;
    const bool enabled = hit_enabled(hit) || selected;
    const bool hot = enabled && (hover_ == hit || pressed_ == hit);
    if (segment) {
        if (selected) {
            p.pill(r, c.seg_on);
        } else if (hot) {
            p.pill(r, c.fill_hi);
        }
        p.text_center(TextStyle::BodyStrong, text, r, selected || hot ? c.ink : c.ink2, enabled ? 1.0f : ui::alpha::disabled);
        return;
    }
    if (primary) {
        p.pill(r, hot ? c.accent_hover : c.accent, enabled ? 1.0f : ui::alpha::disabled);
        p.text_center(TextStyle::BodyStrong, text, r, c.on_accent, enabled ? 1.0f : 0.6f);
    } else {
        p.pill(r, hot ? c.fill_hi : c.fill, enabled ? 1.0f : ui::alpha::disabled);
        p.text_center(TextStyle::BodyStrong, text, r, c.ink, enabled ? 1.0f : ui::alpha::disabled);
    }
}

void DisplaySettings::draw_panel(ui::Painter& p, const Layout& l) {
    const ui::Palette& c = pal();
    const auto P = [this](float dp) { return static_cast<float>(px(dp, dpi_)); };
    p.round(l.panel, P(ui::radius::lg), c.tile);
    const float pl = l.panel.left + P(kTilePad);
    const float pr = l.panel.right - P(kTilePad);
    const float caption_h = text_.metrics(TextStyle::Caption).line_px;
    const bool dragging = dragging_ && drag_moved_;
    const Side side = dragging ? drag_side_ : pending_side_;
    const float inset = P(kSegmentInset);
    const auto track = [&](const D2D1_RECT_F& first, const D2D1_RECT_F& last) {
        p.pill({first.left - inset, first.top - inset, last.right + inset, last.bottom + inset}, c.fill);
    };

    p.text(TextStyle::Caption, L"Other PC is", pl, l.side_label_top, pr - pl, c.ink3);
    track(l.sides[0], l.sides[3]);
    for (int i = 0; i < 4; ++i) {
        draw_button(p, l.sides[i], capitalized(side_word(kSides[i])), kSideHits[i], false, kSides[i] == side);
    }
    if (!layout::kVerticalSidesAccepted) {
        p.text(TextStyle::Caption, L"Top and bottom: next host update.", pl, l.side_note_top, pr - pl, c.ink3);
    }

    p.text(TextStyle::Caption, options_.role ? L"This PC is the" : L"This PC is the (guessed)", pl, l.role_label_top,
           pr - pl, c.ink3);
    track(l.roles[0], l.roles[1]);
    draw_button(p, l.roles[0], L"Host", Hit::RoleHost, false, role_ == Role::Host);
    draw_button(p, l.roles[1], L"Client", Hit::RoleClient, false, role_ == Role::Client);

    // Readouts, as many as fit above the buttons.
    std::wstring state;
    std::uint32_t state_rgb = c.ink;
    if (status_ == Status::Error) {
        state = L"Error";
        state_rgb = c.err;
    } else if (dirty() || side != saved_side_) {  // while dragging: what dropping here would mean
        state = L"Not saved";
        state_rgb = c.warn;
    } else if (status_ == Status::Saved) {
        state = L"Saved";
        state_rgb = c.ok;
    } else {
        state = cfg_.exists && !cfg_.client_side.empty() ? L"Saved" : L"Default";
    }
    struct Row {
        std::wstring label, value;
        std::uint32_t rgb;
        TextStyle style;
    };
    const Row rows[] = {
        {L"Mouse crosses", capitalized(side_word(side)) + L" edge", c.ink, TextStyle::BodyStrong},
        {L"Crossing edge", std::format(L"{} px", layout::outer_edge_length(mons_, side)), c.ink, TextStyle::MonoStrong},
        {L"client_side", L"\"" + side_word(layout::client_side_for(role_, side)) + L"\"", c.ink, TextStyle::MonoStrong},
        {L"Status", state, state_rgb, TextStyle::BodyStrong},
    };
    const float row_h = P(kRowH);
    const float line = static_cast<float>(ui::hairline(dpi_));
    float y = l.rows_top;
    for (const auto& row : rows) {
        if (y + row_h > l.apply.top - P(ui::space::s8)) break;
        p.text(TextStyle::Caption, row.label, pl, std::round(y + (row_h - caption_h) / 2), (pr - pl) / 2, c.ink3);
        p.text_right(row.style, row.value, pr, std::round(y + (row_h - text_.metrics(row.style).line_px) / 2), row.rgb);
        y += row_h;
        if (&row != &rows[std::size(rows) - 1]) p.fill({pl, y - line, pr, y}, c.ink, ui::alpha::line);
    }

    draw_button(p, l.apply, L"Apply", Hit::Apply, true, false);
    draw_button(p, l.revert, L"Revert", Hit::Revert, false, false);
}

void DisplaySettings::draw_footer(ui::Painter& p, const Layout& l) {
    const ui::Palette& c = pal();
    // Status line: what happened, or what to do next.
    std::wstring text = status_text_;
    std::uint32_t rgb = c.ink2;
    const std::wstring other = peer_name();
    const std::wstring this_side = side_word(display::opposite(pending_side_));
    switch (status_) {
    case Status::Error:
        rgb = c.err;
        break;
    case Status::Saved:
        rgb = c.ok;
        text = std::format(L"Saved. Restart Sarke on this PC \u2014 and on {}, this PC must be on the {}.", other, this_side);
        break;
    default:
        if (dirty()) {
            rgb = c.warn;
            text = std::format(L"Not saved yet. Apply writes client_side \"{}\" to config.json, then restart Sarke.",
                               side_word(layout::client_side_for(role_, pending_side_)));
        } else if (!cfg_.exists) {
            text = L"No config.json yet \u2014 Sarke uses \"right\". Apply creates it next to the app.";
        } else {
            text = std::format(L"Each PC keeps its own setting: on {}, this PC must be on the {}.", other, this_side);
        }
        break;
    }
    p.text(TextStyle::Body, text, l.x0, l.status_top, l.x1 - l.x0, rgb);

    const float mono_h = text_.metrics(TextStyle::Mono).line_px;
    const float ty = std::round((l.footer.top + l.footer.bottom - mono_h) / 2);
    p.text(TextStyle::Mono, cfg_.path.wstring(), l.x0, ty, l.identify.left - l.x0 - px(ui::space::s16, dpi_), c.ink3);
    draw_button(p, l.identify, L"Identify", Hit::Identify, false, false);
    // The website's secondary action: blue text with a chevron.
    const bool link_hot = hover_ == Hit::WindowsSettings || pressed_ == Hit::WindowsSettings;
    p.text_center(TextStyle::BodyStrong, L"Windows display settings \u203A", l.windows_settings,
                  link_hot ? c.accent_hover : c.accent_ink);
}

}  // namespace kvm
