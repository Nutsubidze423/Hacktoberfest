// SPDX-License-Identifier: Apache-2.0

#include "device_picker.h"

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
using ui::TextStyle;

constexpr wchar_t kClassName[] = L"SarkeDevicePicker";
constexpr UINT_PTR kOutsideTimer = 1;  // without the focus: watches for a click outside the card
constexpr UINT kOutsideMs = 100;

// Card (dp), like the notification card (docs/DESIGN.md 9.3).
constexpr float kWidth = 380;
constexpr float kShadow = 14;  // transparent room around the card for its shadow
constexpr float kPad = 16;
constexpr float kDot = 7;
constexpr float kHeaderGap = 2;
constexpr float kSection = 14;
constexpr float kRowH = 56;
constexpr float kRowGap = 4;
constexpr float kRowPad = 12;
constexpr float kStatusDot = 9;
constexpr float kSwitchW = 34;
constexpr float kSwitchH = 20;
constexpr float kSwitchGap = 8;
constexpr float kButtonH = 36;
constexpr float kShadowAlpha = 0.045f;  // per shadow ring
constexpr int kShadowRings = 7;

using State = DevicePicker::Row::State;

bool clickable(State s) {
    return s == State::Free;
}

std::uint32_t dot_color(State s) {
    switch (s) {
    case State::Free: return pal().accent_ink;
    case State::Connected: return pal().ok;
    case State::Connecting: return pal().warn;
    case State::Busy: return pal().ink3;
    case State::Older:
    case State::Newer: return pal().warn;
    default: return pal().ink3;
    }
}

const wchar_t* state_text(State s) {
    switch (s) {
    case State::Busy: return L"Busy";
    case State::Connecting: return L"Connecting…";
    case State::Connected: return L"Connected";
    case State::Offline: return L"Not on the network";
    case State::Older: return L"Needs a Sarke update";
    case State::Newer: return L"Update Sarke on this PC";
    default: return L"";
    }
}

bool contains(const D2D1_RECT_F& r, POINT p) {
    const auto x = static_cast<float>(p.x), y = static_cast<float>(p.y);
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

}  // namespace

DevicePicker::~DevicePicker() {
    if (hwnd_) DestroyWindow(hwnd_);
}

bool DevicePicker::ensure_window() {
    if (hwnd_) return true;
    try {
        ui::Graphics::get();
    } catch (const std::exception& e) {
        log::error("picker: {}", e.what());
        return false;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    if (!GetClassInfoExW(GetModuleHandleW(nullptr), kClassName, &wc)) {
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &DevicePicker::wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return false;
    }
    // Per-pixel alpha: the rounded corners and the shadow are really transparent (and click-through).
    CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, kClassName, L"Connect to another PC", WS_POPUP, 0,
                    0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    return hwnd_ != nullptr;  // set in WM_NCCREATE
}

void DevicePicker::show(std::vector<Row> rows, Actions actions, std::optional<POINT> anchor) {
    if (!ensure_window()) return;
    rows_ = std::move(rows);
    actions_ = std::move(actions);
    hover_ = {};
    if (anchor) {
        anchor_ = *anchor;
    } else {
        GetCursorPos(&anchor_);
    }
    layout_at(anchor_);
    render();
    present();
    close_on_blur_ = !anchor;
    had_focus_ = false;
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);  // so a click anywhere else closes it (WM_ACTIVATE)
    had_focus_ = GetForegroundWindow() == hwnd_;
    if (!close_on_blur_ || !had_focus_) SetTimer(hwnd_, kOutsideTimer, kOutsideMs, nullptr);
}

void DevicePicker::update(std::vector<Row> rows) {
    if (!is_open()) return;
    const bool resized = rows.size() != rows_.size() || (rows.empty() != rows_.empty());
    rows_ = std::move(rows);
    if (hover_.row >= rows_.size()) hover_ = {};
    if (resized) layout_at(anchor_);
    render();
    present();
}

void DevicePicker::close() {
    if (hwnd_) KillTimer(hwnd_, kOutsideTimer);
    if (hwnd_ && IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
    hover_ = {};
}

bool DevicePicker::has_ours() const {
    return std::any_of(rows_.begin(), rows_.end(),
                       [](const Row& r) { return r.state == State::Connected || r.state == State::Connecting; });
}

void DevicePicker::layout_at(POINT anchor) {
    const HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor, &mi);
    UINT dpi_x = 96, dpi_y = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) dpi_x = GetDpiForSystem();
    dpi_ = dpi_x;
    if (text_dpi_ != dpi_) {
        text_.create(dpi_);
        text_dpi_ = dpi_;
    }
    const auto line = [&](TextStyle s) { return text_.metrics(s).line_px; };
    const float header = line(TextStyle::Caption) + P(kHeaderGap) + line(TextStyle::Title) + line(TextStyle::Caption);
    const std::size_t n = std::max<std::size_t>(rows_.size(), 1);  // the empty state takes one row
    const float list = static_cast<float>(n) * P(kRowH) + static_cast<float>(n - 1) * P(kRowGap);
    const float footer = has_ours() ? P(kButtonH) : line(TextStyle::Caption);
    const float card_h = P(kPad) + header + P(kSection) + list + P(kSection) + footer + P(kPad);
    const int margin = static_cast<int>(P(kShadow));
    w_ = static_cast<int>(P(kWidth)) + 2 * margin;
    h_ = static_cast<int>(std::ceil(card_h)) + 2 * margin;

    // Centred on the pointer, below it if there is room (the Connect button), else above (the tray).
    const RECT& work = mi.rcWork;
    x_ = std::clamp(static_cast<int>(anchor.x) - w_ / 2, static_cast<int>(work.left) - margin,
                    static_cast<int>(work.right) - w_ + margin);
    y_ = anchor.y + h_ - margin <= work.bottom ? anchor.y - margin + static_cast<int>(P(8))
                                               : anchor.y - h_ + margin - static_cast<int>(P(8));
    y_ = std::clamp(y_, static_cast<int>(work.top) - margin, static_cast<int>(work.bottom) - h_ + margin);
}

D2D1_RECT_F DevicePicker::card() const {
    const float m = P(kShadow);
    return {m, m, static_cast<float>(w_) - m, static_cast<float>(h_) - m};
}

D2D1_RECT_F DevicePicker::row_rect(std::size_t i) const {
    const auto c = card();
    const auto line = [&](TextStyle s) { return text_.metrics(s).line_px; };
    const float top = c.top + P(kPad) + line(TextStyle::Caption) + P(kHeaderGap) + line(TextStyle::Title) +
                      line(TextStyle::Caption) + P(kSection) + static_cast<float>(i) * (P(kRowH) + P(kRowGap));
    return {c.left + P(kPad) - P(4), top, c.right - P(kPad) + P(4), top + P(kRowH)};
}

D2D1_RECT_F DevicePicker::auto_rect(std::size_t i) const {
    const auto r = row_rect(i);
    const float right = r.right - P(kRowPad);
    const float mid = std::round((r.top + r.bottom) / 2);
    return {right - P(kSwitchW), mid - P(kSwitchH) / 2, right, mid + P(kSwitchH) / 2};
}

D2D1_RECT_F DevicePicker::disconnect_rect() const {
    const auto c = card();
    return {c.left + P(kPad), c.bottom - P(kPad) - P(kButtonH), c.right - P(kPad), c.bottom - P(kPad)};
}

DevicePicker::Hit DevicePicker::hit_test(POINT client) const {
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        auto a = auto_rect(i);
        a.left -= P(kSwitchGap);  // a forgiving target
        a.top -= P(6);
        a.bottom += P(6);
        if (rows_[i].paired && contains(a, client)) return {Part::Auto, i};
        if (contains(row_rect(i), client)) return {Part::Row, i};
    }
    if (has_ours() && contains(disconnect_rect(), client)) return {Part::Disconnect, 0};
    return {};
}

void DevicePicker::render() {
    ID2D1RenderTarget* rt = canvas_.begin(w_, h_);
    if (!rt) return;
    const ui::Palette& c = pal();
    ui::Painter p(rt, canvas_.brush(), text_);
    const auto cd = card();
    const float k = P(ui::radius::md);
    const auto line = [&](TextStyle s) { return text_.metrics(s).line_px; };

    // Soft shadow, card, hairline (as the notification card).
    const float step = P(kShadow) / kShadowRings;
    for (int i = kShadowRings; i >= 1; --i) {
        const float o = step * static_cast<float>(i);
        p.round({cd.left - o, cd.top - o + step, cd.right + o, cd.bottom + o + step}, k + o, 0x000000,
                kShadowAlpha * static_cast<float>(kShadowRings - i + 1) / kShadowRings);
    }
    p.round(cd, k, c.tile);
    p.round_stroke(cd, k, static_cast<float>(ui::hairline(dpi_)), c.ink, ui::alpha::line);

    // Header: "● Sarke", the title, what the list is.
    const float x0 = cd.left + P(kPad);
    const float width = cd.right - P(kPad) - x0;
    float y = cd.top + P(kPad);
    const float dot = P(kDot);
    p.circle({x0 + dot / 2, y + line(TextStyle::Caption) / 2}, dot / 2, c.accent_ink);
    p.text(TextStyle::Caption, ui::kAppName, x0 + dot + P(6), y, width, c.ink);
    y += line(TextStyle::Caption) + P(kHeaderGap);
    p.text(TextStyle::Title, L"Connect to another PC", x0, y, width, c.ink);
    y += line(TextStyle::Title);
    p.text(TextStyle::Caption, L"PCs running Sarke on this network", x0, y, width, c.ink3);

    // The PCs.
    if (rows_.empty()) {
        const auto r = row_rect(0);
        p.round(r, P(ui::radius::sm), c.fill);
        const float ty = std::round((r.top + r.bottom - line(TextStyle::BodyStrong) - line(TextStyle::Caption)) / 2);
        p.text(TextStyle::BodyStrong, L"Looking for other PCs…", r.left + P(kRowPad), ty,
               r.right - r.left - 2 * P(kRowPad), c.ink2);
        p.text(TextStyle::Caption, L"Start Sarke on the other PC, on the same Wi-Fi", r.left + P(kRowPad),
               ty + line(TextStyle::BodyStrong), r.right - r.left - 2 * P(kRowPad), c.ink3);
    }
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        const auto r = row_rect(i);
        const bool hot = hover_.row == i && hover_.part == Part::Row && clickable(row.state);
        const bool ours = row.state == State::Connected || row.state == State::Connecting;
        if (hot || ours) p.round(r, P(ui::radius::sm), hot ? c.fill_hi : c.fill);
        const float dim = row.state == State::Busy || row.state == State::Offline || row.state == State::Older ||
                                  row.state == State::Newer
                              ? 0.55f
                              : 1.0f;

        // Status dot, name, IP.
        const float sd = P(kStatusDot);
        const float cx = r.left + P(kRowPad) + sd / 2;
        const float mid = (r.top + r.bottom) / 2;
        p.circle({cx, mid}, sd / 2, dot_color(row.state), dim);
        const float tx = r.left + P(kRowPad) + sd + P(kRowPad);
        const auto a = auto_rect(i);
        // Right side, from the switch leftwards: "Auto", then the state. An unpaired PC has no switch.
        float text_right = a.right;
        if (row.paired) {
            const float auto_label_right = a.left - P(kSwitchGap);
            text_right = p.text_right(TextStyle::Caption, L"Auto", auto_label_right,
                                      std::round(mid - line(TextStyle::Caption) / 2), c.ink3) -
                         P(kRowPad);
        }
        const bool to_pair = !row.paired && row.state == State::Free;
        if (const std::wstring_view st = to_pair ? L"Pair…" : state_text(row.state); !st.empty()) {
            const std::uint32_t ink =
                row.state == State::Connected                                                              ? c.ok
                : row.state == State::Connecting || row.state == State::Older || row.state == State::Newer ? c.warn
                : to_pair ? c.accent_ink
                          : c.ink3;
            text_right =
                p.text_right(TextStyle::Caption, st, text_right, std::round(mid - line(TextStyle::Caption) / 2), ink) -
                P(kRowPad);
        }
        const float name_top = std::round(mid - (line(TextStyle::BodyStrong) + line(TextStyle::Mono)) / 2);
        p.text(TextStyle::BodyStrong, row.name, tx, name_top, std::max(0.0f, text_right - tx), c.ink, dim);
        p.text(TextStyle::Mono, row.ip.empty() ? std::wstring(L"—") : row.ip, tx,
               name_top + line(TextStyle::BodyStrong), std::max(0.0f, text_right - tx), c.ink2, dim);

        // The Auto switch: a pill track and a round knob.
        if (!row.paired) continue;
        const bool auto_hot = hover_.row == i && hover_.part == Part::Auto;
        p.pill(a, row.auto_connect ? (auto_hot ? c.accent_hover : c.accent) : (auto_hot ? c.seg_on : c.fill_hi));
        const float knob = P(kSwitchH) - P(4);
        const float kx = row.auto_connect ? a.right - P(2) - knob / 2 : a.left + P(2) + knob / 2;
        p.circle({kx, (a.top + a.bottom) / 2}, knob / 2, row.auto_connect ? c.on_accent : c.ink2);
    }

    // Footer: Disconnect when a row is ours, else what Auto means.
    if (has_ours()) {
        const auto b = disconnect_rect();
        const bool hot = hover_.part == Part::Disconnect;
        p.pill(b, hot ? c.seg_on : c.fill_hi);
        p.text_center(TextStyle::BodyStrong, L"Disconnect", b, c.ink);
    } else {
        p.text(TextStyle::Caption, L"Auto: Sarke joins that PC by itself whenever it is on the network.", x0,
               cd.bottom - P(kPad) - line(TextStyle::Caption), width, c.ink3);
    }
    canvas_.end();
}

void DevicePicker::present() {
    if (hwnd_) canvas_.present(hwnd_, x_, y_, 255);
}

LRESULT CALLBACK DevicePicker::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<DevicePicker*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;  // CreateWindowExW has not returned yet
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<DevicePicker*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle_message(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT DevicePicker::handle_message(UINT msg, WPARAM wp, LPARAM lp) {
    const HWND hwnd = hwnd_;
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) {
            if (GetForegroundWindow() == hwnd) {  // e.g. clicked: from now on losing the focus closes it
                had_focus_ = true;
                if (close_on_blur_) KillTimer(hwnd, kOutsideTimer);
            }
        } else if (close_on_blur_ && had_focus_) {
            close();  // a click anywhere else
        }
        return 0;
    case WM_TIMER:
        if (wp == kOutsideTimer) {
            const bool pressed =
                (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000;
            POINT pt{};
            GetCursorPos(&pt);
            const HWND under = WindowFromPoint(pt);
            if (pressed && (!under || GetAncestor(under, GA_ROOT) != hwnd)) close();
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) close();
        return 0;
    case WM_MOUSEMOVE: {
        if (!tracking_leave_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            tracking_leave_ = TrackMouseEvent(&tme) != FALSE;
        }
        const Hit h = hit_test({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (!(h == hover_)) {
            hover_ = h;
            render();
            present();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_leave_ = false;
        if (hover_.part != Part::None) {
            hover_ = {};
            render();
            present();
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            const bool hand =
                hover_.part == Part::Auto || hover_.part == Part::Disconnect ||
                (hover_.part == Part::Row && hover_.row < rows_.size() && clickable(rows_[hover_.row].state));
            SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP: {
        const Hit h = hit_test({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        const Actions actions = actions_;  // a copy: the callback may show the picker again
        if (h.part == Part::Auto && h.row < rows_.size() && rows_[h.row].paired) {
            rows_[h.row].auto_connect = !rows_[h.row].auto_connect;  // feels instant; update() confirms
            render();
            present();
            if (actions.toggle_auto) actions.toggle_auto(h.row);
        } else if (h.part == Part::Row && h.row < rows_.size() && clickable(rows_[h.row].state)) {
            close();
            if (actions.connect) actions.connect(h.row);
        } else if (h.part == Part::Disconnect) {
            close();
            if (actions.disconnect) actions.disconnect();
        }
        return 0;
    }
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        if (msg == WM_SETTINGCHANGE && ui::is_theme_change(msg, lp)) ui::refresh_theme();
        if (is_open()) {
            layout_at(anchor_);
            render();
            present();
        }
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
        break;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace kvm
