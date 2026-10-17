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

namespace color = ui::color;
using display::Side;
using layout::Role;
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

// Window (dp).
constexpr float kInitialW = 960;
constexpr float kInitialH = 680;
constexpr float kMinW = 800;
constexpr float kMinH = 620;
constexpr float kStripH = 32;
constexpr float kCaptionButtonW = 46;
constexpr float kCaptionGlyph = 10;
constexpr float kResizeBorder = 6;
constexpr float kLogo = 16;
constexpr float kPanelW = 288;
constexpr float kSegmentH = 32;
constexpr float kApplyH = 40;
constexpr float kRowH = 28;
constexpr float kMapMargin = 40;      // around the drawn desktop, room for the LIVE line and glow
constexpr float kGrid = 32;
constexpr float kEdgeBar = 3;         // the crossing edge
constexpr float kPeerFrame = 2;
constexpr float kStatusSquare = 8;
constexpr float kIdentifySize = 176;  // the popup on each monitor
constexpr float kIdentifyInset = 48;
constexpr float kIdentifyFrame = 4;
constexpr float kIdentifyNumber = 96;
constexpr float kGlow[] = {0.32f, 0.18f, 0.09f, 0.04f};  // edge glow steps, inwards

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

std::wstring upper(std::wstring s) {
    if (!s.empty()) CharUpperBuffW(s.data(), static_cast<DWORD>(s.size()));
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

// One line of text centred in r.
void text_center(ui::Painter& p, TextStyle style, std::wstring_view s, const D2D1_RECT_F& r, std::uint32_t rgb) {
    const float w = std::round(p.styles().measure(style, s));
    const float line = p.styles().metrics(style).line_px;
    p.text(style, s, std::round((r.left + r.right - w) / 2), std::round((r.top + r.bottom - line) / 2),
           w + ui::text_style_spec(style).size_dp, rgb);
}

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

class IdentifyPopup {
public:
    IdentifyPopup(const display::Monitor& m, int number) : dpi_(m.dpi ? m.dpi : 96) {
        number_ = std::to_wstring(number);
        detail_ = std::format(L"{}{}{}", size_text(m.rect.width(), m.rect.height()), ui::kSeparator, percent(m.scale));
        if (!register_class(kIdentifyClassName, &IdentifyPopup::wnd_proc)) return;
        const int size = px(kIdentifySize, dpi_);
        const int inset = px(kIdentifyInset, dpi_);
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kIdentifyClassName, L"", WS_POPUP,
                                m.work.left + inset, m.work.bottom - inset - size, size, size, nullptr, nullptr,
                                GetModuleHandleW(nullptr), this);
        if (!hwnd_) return;
        const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_DONOTROUND;
        DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
    ~IdentifyPopup() {
        if (hwnd_) DestroyWindow(hwnd_);
    }
    IdentifyPopup(const IdentifyPopup&) = delete;
    IdentifyPopup& operator=(const IdentifyPopup&) = delete;

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        }
        auto* self = reinterpret_cast<IdentifyPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
        case WM_PAINT:
            if (self) self->paint(hwnd);
            ValidateRect(hwnd, nullptr);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
            ShowWindow(hwnd, SW_HIDE);  // click it away
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (self) self->hwnd_ = nullptr;
            break;
        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void paint(HWND hwnd) {
        try {
            if (!rt_) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.0f, 96.0f);
                if (FAILED(ui::Graphics::get().d2d()->CreateHwndRenderTarget(
                        props, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)), &rt_))) {
                    return;
                }
                rt_->CreateSolidColorBrush(ui::to_color(color::text_hi), &brush_);
                text_.create(dpi_);
                auto& g = ui::Graphics::get();
                g.dwrite()->CreateTextFormat(g.family(), g.fonts(), DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(px(kIdentifyNumber, dpi_)),
                                             L"en-us", &big_);
            }
            const auto size = rt_->GetSize();
            const D2D1_RECT_F all{0, 0, size.width, size.height};
            rt_->BeginDraw();
            rt_->Clear(ui::to_color(color::bg0));
            ui::Painter p(rt_.Get(), brush_.Get(), text_);
            p.frame(all, static_cast<float>(px(kIdentifyFrame, dpi_)), color::accent);
            const float pad = static_cast<float>(px(ui::space::s16, dpi_));
            if (big_) {
                brush_->SetColor(ui::to_color(color::text_hi));
                const D2D1_RECT_F box{pad, pad / 2, size.width - pad, size.height - pad};
                rt_->DrawTextW(number_.c_str(), static_cast<UINT32>(number_.size()), big_.Get(), box, brush_.Get());
            }
            const float line = text_.metrics(TextStyle::Label).line_px;
            p.text(TextStyle::Label, detail_, pad, size.height - pad - line, size.width - 2 * pad, color::text_mid);
            p.text(TextStyle::Label, ui::kAppName, pad, size.height - pad - 2 * line - static_cast<float>(px(4, dpi_)),
                   size.width - 2 * pad, color::accent);
            if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) rt_.Reset();
        } catch (const std::exception& e) {
            log::error("displays: identify: {}", e.what());
        }
    }

    HWND hwnd_ = nullptr;
    UINT dpi_;
    std::wstring number_, detail_;
    ui::ComPtr<ID2D1HwndRenderTarget> rt_;
    ui::ComPtr<ID2D1SolidColorBrush> brush_;
    ui::ComPtr<IDWriteTextFormat> big_;
    ui::TextStyles text_;
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
    role_ = options_.role ? *options_.role : layout::guess_role(cfg_.host_ip);
    if (!peer_set_) {
        if (auto p = layout::load_peer()) peer_ = *p;
    }
    reload_config();

    const UINT system_dpi = GetDpiForSystem();
    const HWND hwnd = CreateWindowExW(0, kClassName, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                      px(kInitialW, system_dpi), px(kInitialH, system_dpi), nullptr, nullptr,
                                      GetModuleHandleW(nullptr), this);
    if (!hwnd) return false;
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));

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
    const auto label_h = text_.metrics(TextStyle::Label).line_px;
    const auto body_h = text_.metrics(TextStyle::Body).line_px;

    Layout l;
    l.strip = {0, 0, w, P(kStripH)};
    l.close = {w - P(kCaptionButtonW), 0, w, l.strip.bottom};
    l.minimize = {l.close.left - P(kCaptionButtonW), 0, l.close.left, l.strip.bottom};
    l.x0 = P(ui::space::s32);
    l.x1 = w - P(ui::space::s32);

    float y = l.strip.bottom + P(ui::space::s24);
    l.label_top = y;
    y += label_h + P(ui::space::s8);
    l.title_top = y;
    y += text_.metrics(TextStyle::Display).line_px + P(ui::space::s8);
    l.body_top = y;
    y += 2 * body_h + P(ui::space::s24);
    const float main_top = y;

    const float footer_bottom = h - P(ui::space::s24);
    l.footer = {l.x0, footer_bottom - P(kSegmentH), l.x1, footer_bottom};
    l.status_top = l.footer.top - P(ui::space::s16) - body_h;
    const float main_bottom = std::max(main_top + P(ui::space::s48 * 3), l.status_top - P(ui::space::s16));
    l.panel = {l.x1 - P(kPanelW), main_top, l.x1, main_bottom};
    l.map = {l.x0, main_top, l.panel.left - P(ui::space::s24), main_bottom};

    const float pl = l.panel.left + P(ui::space::s16);
    const float pr = l.panel.right - P(ui::space::s16);
    const float gap = P(ui::space::s4);
    float py = l.panel.top + P(ui::space::s16);
    l.side_label_top = py;
    py += label_h + P(ui::space::s8);
    const float seg_w = std::floor((pr - pl - 3 * gap) / 4);
    for (int i = 0; i < 4; ++i) {
        const float left = pl + static_cast<float>(i) * (seg_w + gap);
        l.sides[i] = {left, py, i == 3 ? pr : left + seg_w, py + P(kSegmentH)};
    }
    py += P(kSegmentH) + P(ui::space::s8);
    l.side_note_top = py;
    py += label_h + P(ui::space::s16);
    l.role_label_top = py;
    py += label_h + P(ui::space::s8);
    const float half = std::floor((pr - pl - gap) / 2);
    l.roles[0] = {pl, py, pl + half, py + P(kSegmentH)};
    l.roles[1] = {pl + half + gap, py, pr, py + P(kSegmentH)};
    py += P(kSegmentH) + P(ui::space::s16);
    l.rows_top = py;
    const float bt = l.panel.bottom - P(ui::space::s16) - P(kApplyH);
    const float bhalf = std::floor((pr - pl - P(ui::space::s8)) / 2);
    l.apply = {pl, bt, pl + bhalf, bt + P(kApplyH)};
    l.revert = {pl + bhalf + P(ui::space::s8), bt, pr, bt + P(kApplyH)};

    const float pad2 = 2 * P(ui::space::s16);
    const float ws_w = std::round(text_.measure(TextStyle::Label, L"Windows display settings")) + pad2;
    l.windows_settings = {l.x1 - ws_w, l.footer.top, l.x1, l.footer.bottom};
    const float id_w = std::round(text_.measure(TextStyle::Label, L"Identify")) + pad2;
    l.identify = {l.windows_settings.left - P(ui::space::s8) - id_w, l.footer.top,
                  l.windows_settings.left - P(ui::space::s8), l.footer.bottom};
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
    rt_->CreateSolidColorBrush(ui::to_color(color::text_hi), &brush_);
    return true;
}

void DisplaySettings::render() {
    if (!ensure_render_target()) return;
    const Layout l = compute_layout();
    rt_->BeginDraw();
    rt_->Clear(ui::to_color(color::bg0));
    ui::Painter p(rt_.Get(), brush_.Get(), text_);
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
    const auto P = [this](float dp) { return static_cast<float>(px(dp, dpi_)); };
    // Logo: the same cells as the icon (docs/DESIGN.md section 9).
    const float logo = P(kLogo);
    const float lx = P(ui::space::s12);
    const float ly = std::round((height(l.strip) - logo) / 2);
    const float unit = logo / ui::logo::grid;
    for (const auto& c : ui::logo::cells) {
        p.fill({std::round(lx + c.x * unit), std::round(ly + c.y * unit), std::round(lx + (c.x + c.w) * unit),
                std::round(ly + (c.y + c.h) * unit)},
               c.rgb);
    }
    const float label_h = text_.metrics(TextStyle::Label).line_px;
    const float ty = std::round((height(l.strip) - label_h) / 2);
    float x = lx + logo + P(ui::space::s8);
    x += p.text(TextStyle::Label, ui::kAppName, x, ty, l.minimize.left - x, color::text_hi);
    p.text(TextStyle::Label, std::wstring(ui::kSeparator) + L"Displays", x, ty, l.minimize.left - x, color::text_lo);

    const float glyph = P(kCaptionGlyph);
    const float line = static_cast<float>(ui::hairline(dpi_));
    const bool min_hot = hover_ == Hit::Minimize || pressed_ == Hit::Minimize;
    const bool close_hot = hover_ == Hit::Close || pressed_ == Hit::Close;
    if (min_hot) p.fill(l.minimize, color::bg2);
    if (close_hot) p.fill(l.close, color::err);
    const float mcx = std::round((l.minimize.left + l.minimize.right - glyph) / 2);
    const float mcy = std::round((l.minimize.top + l.minimize.bottom) / 2);
    p.fill({mcx, mcy, mcx + glyph, mcy + line}, min_hot ? color::text_hi : color::text_mid);
    const float cx = std::round((l.close.left + l.close.right - glyph) / 2);
    const float cy = std::round((l.close.top + l.close.bottom - glyph) / 2);
    const auto close_rgb = close_hot ? color::text_hi : color::text_mid;
    p.diagonal({cx, cy}, {cx + glyph, cy + glyph}, line, close_rgb);
    p.diagonal({cx + glyph, cy}, {cx, cy + glyph}, line, close_rgb);
}

void DisplaySettings::draw_header(ui::Painter& p, const Layout& l) {
    const float w = l.x1 - l.x0;
    p.text(TextStyle::Label, L"Settings · Displays", l.x0, l.label_top, w, color::text_lo);
    p.text(TextStyle::Display, L"Where is the other PC?", l.x0, l.title_top, w, color::text_hi);
    const float body_h = text_.metrics(TextStyle::Body).line_px;
    p.text(TextStyle::Body, L"Drag the other PC to the side of your screens where it really stands.", l.x0, l.body_top,
           w, color::text_mid);
    p.text(TextStyle::Body, L"The orange edge is where your mouse crosses over. Arrow keys work too.", l.x0,
           l.body_top + body_h, w, color::text_mid);
}

void DisplaySettings::draw_map(ui::Painter& p, const Layout& l) {
    const auto P = [this](float dp) { return static_cast<float>(px(dp, dpi_)); };
    const D2D1_RECT_F& map = l.map;
    p.fill(map, color::bg1);
    // Grid.
    const float grid = P(kGrid);
    const float line = static_cast<float>(ui::hairline(dpi_));
    for (float x = map.left + grid; x < map.right; x += grid) p.fill({x, map.top, x + line, map.bottom}, color::line, 0.55f);
    for (float y = map.top + grid; y < map.bottom; y += grid) p.fill({map.left, y, map.right, y + line}, color::line, 0.55f);

    const float label_h = text_.metrics(TextStyle::Label).line_px;
    const float pad = P(ui::space::s12);
    if (mons_.empty()) {
        text_center(p, TextStyle::Label, L"No monitors found", map, color::text_lo);
        return;
    }
    rt_->PushAxisAlignedClip(map, D2D1_ANTIALIAS_MODE_ALIASED);
    const MapTransform t = map_transform(l);
    const bool dragging = dragging_ && drag_moved_;
    const Side side = dragging ? drag_side_ : pending_side_;

    // Where the other PC can go, while dragging.
    if (dragging) {
        for (Side s : kSides) {
            if (!layout::side_accepted(s)) continue;
            const auto zone = t.to_client(layout::peer_rect(mons_, s, peer_w(), peer_h()));
            if (s == drag_side_) {
                p.fill(zone, color::accent, 0.10f);
                p.frame(zone, line, color::accent, 0.7f);
            } else {
                p.frame(zone, line, color::line_strong);
            }
        }
    }

    // This PC's monitors.
    const float gap = std::max(1.0f, std::floor(P(2) / 2));
    for (std::size_t i = 0; i < mons_.size(); ++i) {
        const auto& m = mons_[i];
        auto r = t.to_client(m.rect);
        r = {r.left + gap, r.top + gap, r.right - gap, r.bottom - gap};
        p.fill(r, color::bg2);
        p.frame(r, line, color::line_strong);
        const float tx = r.left + pad;
        const float tw = width(r) - 2 * pad;
        float ty = r.top + P(ui::space::s8);
        const float display_h = text_.metrics(TextStyle::Display).line_px;
        if (ty + display_h <= r.bottom - P(ui::space::s4)) {
            p.text(TextStyle::Display, std::to_wstring(monitor_number(m, i)), tx, ty, tw, color::text_hi);
            ty += display_h + P(ui::space::s4);
        }
        if (ty + label_h <= r.bottom - P(ui::space::s4)) {
            p.text(TextStyle::Label, size_text(m.rect.width(), m.rect.height()), tx, ty, tw, color::text_mid);
            ty += label_h;
        }
        if (ty + label_h <= r.bottom - P(ui::space::s4)) {
            p.text(TextStyle::Label, m.primary ? percent(m.scale) + ui::kSeparator + L"main" : percent(m.scale), tx, ty,
                   tw, color::text_lo);
        }
    }

    // The crossing edge: a bar on the edge and a glow into this PC's screens.
    const float bar = P(kEdgeBar);
    for (const auto& piece : layout::outer_edge_pieces(mons_, side)) {
        const auto e = t.to_client(piece);
        for (int step = -1; step < static_cast<int>(std::size(kGlow)); ++step) {
            const float a = static_cast<float>(step + 1) * bar;  // distance from the edge, inwards
            const float b = a + bar;
            D2D1_RECT_F r{};
            switch (side) {
            case Side::Left: r = {e.left + a, e.top, e.left + b, e.bottom}; break;
            case Side::Right: r = {e.right - b, e.top, e.right - a, e.bottom}; break;
            case Side::Top: r = {e.left, e.top + a, e.right, e.top + b}; break;
            case Side::Bottom: r = {e.left, e.bottom - b, e.right, e.bottom - a}; break;
            }
            p.fill(r, color::accent, step < 0 ? 1.0f : kGlow[step]);
        }
    }

    // The other PC.
    const auto box = peer_box(l, t);
    const bool hot = dragging_ || hover_ == Hit::Peer;
    p.fill(box, color::bg2, dragging ? 0.92f : 1.0f);
    p.frame(box, P(kPeerFrame), color::accent, hot ? 1.0f : 0.85f);
    {
        const float tx = box.left + pad;
        const float tw = width(box) - 2 * pad;
        float ty = box.top + P(ui::space::s8);
        const float readout_h = text_.metrics(TextStyle::Readout).line_px;
        const auto fits = [&](float h) { return ty + h <= box.bottom - P(ui::space::s4); };
        if (fits(label_h)) {
            p.text(TextStyle::Label, L"Other PC", tx, ty, tw, color::accent);
            ty += label_h + P(ui::space::s4);
        }
        if (fits(readout_h)) {
            p.text(TextStyle::Readout, peer_.known() ? widen(peer_.hostname) : std::wstring(ui::kUnknown), tx, ty, tw,
                   color::text_hi);
            ty += readout_h;
        }
        if (fits(label_h)) {
            p.text(TextStyle::Label,
                   peer_.known() ? size_text(peer_w(), peer_h()) + ui::kSeparator + percent(peer_.scale)
                                 : std::wstring(L"size not known yet"),
                   tx, ty, tw, color::text_mid);
            ty += label_h;
        }
        if (fits(label_h)) {
            const bool live = peer_live_ && peer_.known();
            p.text(TextStyle::Label, live ? L"connected" : peer_.known() ? L"last seen" : L"not connected yet", tx, ty, tw,
                   live ? color::ok : color::text_lo);
        }
    }

    // Live detection line.
    const display::Rect desk = display::virtual_desktop(mons_);
    const float sq = P(kStatusSquare);
    const float ly = map.top + pad;
    p.fill({map.left + pad, std::round(ly + (label_h - sq) / 2), map.left + pad + sq, std::round(ly + (label_h - sq) / 2) + sq},
           color::ok);
    p.text(TextStyle::Label,
           std::format(L"Live{}{} monitor{}{}{}", ui::kSeparator, mons_.size(), mons_.size() == 1 ? L"" : L"s",
                       ui::kSeparator, size_text(desk.width(), desk.height())),
           map.left + pad + sq + P(ui::space::s8), ly, width(map) - 3 * pad - sq, color::text_lo);
    rt_->PopAxisAlignedClip();
}

void DisplaySettings::draw_button(ui::Painter& p, const D2D1_RECT_F& r, std::wstring_view text, Hit hit, bool primary,
                                  bool selected) {
    const bool enabled = hit_enabled(hit) || selected;
    const bool hot = enabled && (hover_ == hit || pressed_ == hit);
    std::uint32_t fill = color::bg2;
    std::uint32_t ink = color::text_mid;
    float ink_alpha = 1.0f;
    if (selected || (primary && enabled)) {
        fill = color::accent;
        ink = color::on_accent;
    } else if (!enabled) {
        ink = color::text_lo;
        ink_alpha = 0.5f;
    } else if (hot) {
        fill = color::line_strong;
        ink = color::text_hi;
    } else if (primary || hit == Hit::Revert || hit == Hit::Identify || hit == Hit::WindowsSettings) {
        ink = color::text_hi;
    }
    p.fill(r, fill);
    if ((selected || primary) && hot && !selected) p.fill(r, color::text_hi, 0.12f);
    if (ink_alpha < 1.0f) {
        // Dimmed label: mix towards the fill instead of real alpha (ClearType text needs opaque ink).
        const auto mix = [&](int shift) {
            const int a = (ink >> shift) & 0xFF;
            const int b = (fill >> shift) & 0xFF;
            return static_cast<std::uint32_t>(std::lround(b + (a - b) * ink_alpha)) << shift;
        };
        ink = mix(16) | mix(8) | mix(0);
    }
    text_center(p, TextStyle::Label, text, r, ink);
}

void DisplaySettings::draw_panel(ui::Painter& p, const Layout& l) {
    const auto P = [this](float dp) { return static_cast<float>(px(dp, dpi_)); };
    p.fill(l.panel, color::bg1);
    const float pl = l.panel.left + P(ui::space::s16);
    const float pr = l.panel.right - P(ui::space::s16);
    const float label_h = text_.metrics(TextStyle::Label).line_px;
    const bool dragging = dragging_ && drag_moved_;
    const Side side = dragging ? drag_side_ : pending_side_;

    p.text(TextStyle::Label, L"Other PC is", pl, l.side_label_top, pr - pl, color::text_lo);
    for (int i = 0; i < 4; ++i) {
        draw_button(p, l.sides[i], side_word(kSides[i]), kSideHits[i], false, kSides[i] == side);
    }
    if (!layout::kVerticalSidesAccepted) {
        p.text(TextStyle::Label, L"Top / bottom: next host update", pl, l.side_note_top, pr - pl, color::text_lo);
    }

    p.text(TextStyle::Label, options_.role ? L"This PC is the" : L"This PC is the (guessed)", pl, l.role_label_top,
           pr - pl, color::text_lo);
    draw_button(p, l.roles[0], L"Host", Hit::RoleHost, false, role_ == Role::Host);
    draw_button(p, l.roles[1], L"Client", Hit::RoleClient, false, role_ == Role::Client);

    // Readouts, as many as fit above the buttons.
    struct Row {
        std::wstring label, value;
        std::uint32_t rgb;
    };
    std::wstring state;
    std::uint32_t state_rgb = color::text_hi;
    if (status_ == Status::Error) {
        state = L"ERROR";
        state_rgb = color::err;
    } else if (dirty() || side != saved_side_) {  // while dragging: what dropping here would mean
        state = L"NOT SAVED";
        state_rgb = color::warn;
    } else if (status_ == Status::Saved) {
        state = L"SAVED";
        state_rgb = color::ok;
    } else {
        state = cfg_.exists && !cfg_.client_side.empty() ? L"SAVED" : L"DEFAULT";
    }
    const Row rows[] = {
        {L"Mouse crosses", upper(side_word(side)) + L" EDGE", color::text_hi},
        {L"Crossing edge", std::format(L"{} PX", layout::outer_edge_length(mons_, side)), color::text_hi},
        {L"client_side", L"\"" + side_word(layout::client_side_for(role_, side)) + L"\"", color::text_hi},
        {L"Status", state, state_rgb},
    };
    const float row_h = P(kRowH);
    const float line = static_cast<float>(ui::hairline(dpi_));
    const float readout_h = text_.metrics(TextStyle::Readout).line_px;
    float y = l.rows_top;
    for (const auto& row : rows) {
        if (y + row_h > l.apply.top - P(ui::space::s8)) break;
        p.text(TextStyle::Label, row.label, pl, std::round(y + (row_h - label_h) / 2), (pr - pl) / 2, color::text_lo);
        p.text_right(TextStyle::Readout, row.value, pr, std::round(y + (row_h - readout_h) / 2), row.rgb);
        y += row_h;
        if (&row != &rows[std::size(rows) - 1]) p.fill({pl, y - line, pr, y}, color::line);
    }

    draw_button(p, l.apply, L"Apply", Hit::Apply, true, false);
    draw_button(p, l.revert, L"Revert", Hit::Revert, false, false);
}

void DisplaySettings::draw_footer(ui::Painter& p, const Layout& l) {
    // Status line: what happened, or what to do next.
    std::wstring text = status_text_;
    std::uint32_t rgb = color::text_mid;
    const std::wstring other = peer_name();
    const std::wstring this_side = side_word(display::opposite(pending_side_));
    switch (status_) {
    case Status::Error:
        rgb = color::err;
        break;
    case Status::Saved:
        rgb = color::ok;
        text = std::format(L"Saved. Restart Sarke on this PC - and on {} this PC must be on the {}.", other, this_side);
        break;
    default:
        if (dirty()) {
            rgb = color::warn;
            text = std::format(L"Not saved yet. Apply writes client_side \"{}\" to config.json, then restart Sarke.",
                               side_word(layout::client_side_for(role_, pending_side_)));
        } else if (!cfg_.exists) {
            text = std::format(L"No config.json yet - Sarke uses \"right\". Apply creates it next to the app.");
        } else {
            text = std::format(L"Each PC keeps its own setting: on {}, this PC must be on the {}.", other, this_side);
        }
        break;
    }
    p.text(TextStyle::Body, text, l.x0, l.status_top, l.x1 - l.x0, rgb);

    const float body_h = text_.metrics(TextStyle::Body).line_px;
    const float ty = std::round((l.footer.top + l.footer.bottom - body_h) / 2);
    p.text(TextStyle::Body, L"config: " + cfg_.path.wstring(), l.x0, ty, l.identify.left - l.x0 - px(ui::space::s16, dpi_),
           color::text_lo);
    draw_button(p, l.identify, L"Identify", Hit::Identify, false, false);
    draw_button(p, l.windows_settings, L"Windows display settings", Hit::WindowsSettings, false, false);
}

}  // namespace kvm
