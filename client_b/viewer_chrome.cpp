// SPDX-License-Identifier: Apache-2.0

#include "viewer_chrome.h"

#include <algorithm>
#include <cmath>

namespace kvm::viewer {

namespace {

namespace vt = ui::viewer;
using ui::pal;
using ui::px;
using ui::TextStyle;

bool contains(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

float P(float dp, UINT dpi) {
    return static_cast<float>(px(dp, dpi));
}

void draw_caption_glyph(ui::Painter& p, StripButton button, const D2D1_RECT_F& r, bool maximized, std::uint32_t fg,
                        std::uint32_t bg, UINT dpi) {
    const float g = P(vt::caption_glyph, dpi);
    const auto hair = static_cast<float>(ui::hairline(dpi));
    const float gx = std::floor(r.left + (r.right - r.left - g) / 2);
    const float gy = std::floor(r.top + (r.bottom - r.top - g) / 2);
    const float k = P(2, dpi);  // Windows 11 glyphs have softly rounded squares

    switch (button) {
    case StripButton::Minimize: {
        const float y = std::floor(gy + g / 2);
        p.fill({gx, y, gx + g, y + hair}, fg);
        break;
    }
    case StripButton::Maximize:
        if (maximized) {
            const float off = P(vt::restore_offset, dpi);
            p.round_stroke({gx + off, gy, gx + g, gy + g - off}, k, hair, fg);
            p.round({gx, gy + off, gx + g - off, gy + g}, k, bg);
            p.round_stroke({gx, gy + off, gx + g - off, gy + g}, k, hair, fg);
        } else {
            p.round_stroke({gx, gy, gx + g, gy + g}, k, hair, fg);
        }
        break;
    case StripButton::Close:
        p.diagonal({gx, gy}, {gx + g, gy + g}, hair, fg);
        p.diagonal({gx + g, gy}, {gx, gy + g}, hair, fg);
        break;
    default:
        break;
    }
}

}  // namespace

StripLayout layout_strip(float client_w, UINT dpi, const ui::TextStyles& text) {
    const float h = P(vt::strip_h, dpi);
    const float caption_w = P(vt::caption_button_w, dpi);
    const float pill_h = P(vt::pill_h, dpi);
    const float pad = P(vt::pill_pad, dpi);
    const float gap = P(vt::toggle_gap, dpi);
    const float top = std::floor((h - pill_h) / 2);
    // Both toggles as wide as their widest label, so "Fit" <-> "1:1" doesn't move anything.
    const float toggle_w = std::round(std::max({text.measure(TextStyle::BodyStrong, L"HUD"),
                                                text.measure(TextStyle::BodyStrong, L"Fit"),
                                                text.measure(TextStyle::BodyStrong, L"1:1")})) + 2 * pad;

    StripLayout l;
    l.strip = {0, 0, client_w, h};
    // Right to left: close, maximize/restore, minimize, 8 dp, Fit, 4 dp, HUD.
    float right = client_w;
    l.close = {right - caption_w, 0, right, h};
    right -= caption_w;
    l.maximize = {right - caption_w, 0, right, h};
    right -= caption_w;
    l.minimize = {right - caption_w, 0, right, h};
    right -= caption_w + P(ui::space::s8, dpi);
    l.fit = {right - toggle_w, top, right, top + pill_h};
    right -= toggle_w + gap;
    l.hud = {right - toggle_w, top, right, top + pill_h};
    return l;
}

StripButton hit_test(const StripLayout& l, float x, float y) {
    if (!contains(l.strip, x, y)) {
        return StripButton::None;
    }
    if (contains(l.close, x, y)) return StripButton::Close;
    if (contains(l.maximize, x, y)) return StripButton::Maximize;
    if (contains(l.minimize, x, y)) return StripButton::Minimize;
    if (contains(l.fit, x, y)) return StripButton::Fit;
    if (contains(l.hud, x, y)) return StripButton::Hud;
    return StripButton::None;
}

void draw_strip(ui::Painter& p, const StripLayout& l, const StripModel& m, UINT dpi) {
    const ui::Palette& c = pal();
    const auto& text = p.styles();
    const float h = l.strip.bottom;

    // Logo + "Sarke".
    const float logo = P(vt::logo, dpi);
    const float lx = P(vt::logo_x, dpi);
    const float ly = std::floor((h - logo) / 2);
    p.logo({lx, ly, lx + logo, ly + logo});
    float x = lx + logo + P(ui::space::s8, dpi);
    const float name_top = std::floor((h - text.metrics(TextStyle::BodyStrong).line_px) / 2);
    x += std::round(p.text(TextStyle::BodyStrong, ui::kAppName, x, name_top, l.hud.left - x, c.ink)) + P(ui::space::s12, dpi);

    // Status pill: dot + state word.
    const float pill_h = P(vt::pill_h, dpi);
    const float pad = P(vt::pill_pad, dpi);
    const float dot = P(vt::status_dot, dpi);
    const float word_w = std::round(text.measure(TextStyle::Caption, m.state_word));
    const float pill_w = pad + dot + P(6, dpi) + word_w + pad;
    const float top = std::floor((h - pill_h) / 2);
    if (x + pill_w < l.hud.left - P(ui::space::s8, dpi)) {
        const D2D1_RECT_F pill{x, top, x + pill_w, top + pill_h};
        p.pill(pill, c.tile);
        if (m.status_visible) {
            p.circle({x + pad + dot / 2, top + pill_h / 2}, dot / 2, m.status_color);
        }
        p.text(TextStyle::Caption, m.state_word, x + pad + dot + P(6, dpi),
               std::floor(top + (pill_h - text.metrics(TextStyle::Caption).line_px) / 2), word_w + P(4, dpi), c.ink2);
        x += pill_w + P(ui::space::s12, dpi);
    }

    // Title of what is streaming, up to the toggles.
    if (!m.title.empty()) {
        p.text(TextStyle::Body, m.title, x, std::floor((h - text.metrics(TextStyle::Body).line_px) / 2),
               l.hud.left - P(ui::space::s12, dpi) - x, c.ink2);
    }

    // Toggles: pills; on = filled, off = text only, hover = tile.
    const auto toggle = [&](StripButton b, const D2D1_RECT_F& r, std::wstring_view label, bool on) {
        const bool hot = m.hover == b || m.pressed == b;
        if (on) {
            p.pill(r, hot ? c.fill_hi : c.fill);
        } else if (hot) {
            p.pill(r, c.tile);
        }
        p.text_center(TextStyle::BodyStrong, label, r, on || hot ? c.ink : c.ink2);
    };
    toggle(StripButton::Hud, l.hud, L"HUD", m.hud_on);
    toggle(StripButton::Fit, l.fit, m.one_to_one ? L"1:1" : L"Fit", m.one_to_one);

    // Caption buttons: hover fill tile (close: err with a white glyph), like Windows 11.
    for (const StripButton b : {StripButton::Minimize, StripButton::Maximize, StripButton::Close}) {
        const D2D1_RECT_F& r = b == StripButton::Minimize ? l.minimize : b == StripButton::Maximize ? l.maximize : l.close;
        const bool hot = m.hover == b || m.pressed == b;
        const std::uint32_t bg = hot ? (b == StripButton::Close ? ui::kCloseHover : c.tile) : c.bg;
        if (hot) {
            p.fill(r, bg);
        }
        const std::uint32_t fg = hot ? (b == StripButton::Close ? ui::kCloseGlyph : c.ink) : c.ink2;
        draw_caption_glyph(p, b, r, m.maximized, fg, bg, dpi);
    }
}

}  // namespace kvm::viewer
