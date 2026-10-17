#include "viewer_chrome.h"

#include <cmath>

namespace kvm::viewer {

namespace {

namespace vt = ui::viewer;
namespace color = ui::color;
using ui::px;
using ui::TextStyle;

bool contains(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// Top of a line box of `style` vertically centered in [top, bottom).
float centered_line_top(const ui::Painter& p, TextStyle style, float top, float bottom) {
    return std::floor(top + (bottom - top - p.styles().metrics(style).line_px) / 2);
}

void draw_caption_glyph(ui::Painter& p, StripButton button, const D2D1_RECT_F& r, bool maximized, std::uint32_t fg,
                        std::uint32_t bg, UINT dpi) {
    const auto g = static_cast<float>(px(vt::caption_glyph, dpi));
    const auto hair = static_cast<float>(ui::hairline(dpi));
    const float gx = std::floor(r.left + (r.right - r.left - g) / 2);
    const float gy = std::floor(r.top + (r.bottom - r.top - g) / 2);

    switch (button) {
    case StripButton::Minimize: {
        const float y = std::floor(gy + g / 2);
        p.fill({gx, y, gx + g, y + hair}, fg);
        break;
    }
    case StripButton::Maximize:
        if (maximized) {
            // Restore: back square offset up-right, front square down-left on top of it.
            const auto off = static_cast<float>(px(vt::restore_offset, dpi));
            p.frame({gx + off, gy, gx + g, gy + g - off}, hair, fg);
            p.fill({gx, gy + off, gx + g - off, gy + g}, bg);
            p.frame({gx, gy + off, gx + g - off, gy + g}, hair, fg);
        } else {
            p.frame({gx, gy, gx + g, gy + g}, hair, fg);
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

StripLayout layout_strip(float client_w, UINT dpi) {
    const auto h = static_cast<float>(px(vt::strip_h, dpi));
    const auto caption_w = static_cast<float>(px(vt::caption_button_w, dpi));
    const auto text_w = static_cast<float>(px(vt::text_button_w, dpi));
    const auto gap = static_cast<float>(px(ui::space::s8, dpi));

    StripLayout l;
    l.strip = {0, 0, client_w, h};
    // Right to left: close, maximize/restore, minimize, gap, FIT, HUD.
    float right = client_w;
    l.close = {right - caption_w, 0, right, h};
    right -= caption_w;
    l.maximize = {right - caption_w, 0, right, h};
    right -= caption_w;
    l.minimize = {right - caption_w, 0, right, h};
    right -= caption_w + gap;
    l.fit = {right - text_w, 0, right, h};
    right -= text_w;
    l.hud = {right - text_w, 0, right, h};
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
    const auto hair = static_cast<float>(ui::hairline(dpi));
    p.fill(l.strip, color::bg0);

    // Status square.
    const auto sq = static_cast<float>(px(vt::status_square, dpi));
    const auto sq_x = static_cast<float>(px(vt::status_square_x, dpi));
    const float sq_y = std::floor((l.strip.bottom - sq) / 2);
    if (m.status_visible) {
        p.fill({sq_x, sq_y, sq_x + sq, sq_y + sq}, m.status_color);
    }

    // State word, 8 dp gap, title (ellipsis before the buttons).
    const auto word_x = static_cast<float>(px(vt::state_word_x, dpi));
    const auto gap = static_cast<float>(px(ui::space::s8, dpi));
    const float word_w = p.text(TextStyle::Label, m.state_word, word_x, centered_line_top(p, TextStyle::Label, 0, l.strip.bottom),
                                l.hud.left - word_x, color::text_lo);
    const float title_x = std::round(word_x + word_w + gap);
    p.text(TextStyle::Body, m.title, title_x, centered_line_top(p, TextStyle::Body, 0, l.strip.bottom),
           l.hud.left - gap - title_x, color::text_hi);

    // Text buttons: inactive textMid, active accent; hover/pressed fill bg2.
    const auto text_button = [&](StripButton b, const D2D1_RECT_F& r, std::wstring_view label, bool active) {
        const bool hot = m.hover == b || m.pressed == b;
        if (hot) {
            p.fill(r, color::bg2);
        }
        const std::uint32_t fg = active ? color::accent : (hot ? color::text_hi : color::text_mid);
        const float w = std::round(p.styles().measure(TextStyle::Label, label));
        p.text(TextStyle::Label, label, std::floor(r.left + (r.right - r.left - w) / 2),
               centered_line_top(p, TextStyle::Label, r.top, r.bottom), r.right - r.left, fg);
    };
    text_button(StripButton::Hud, l.hud, L"HUD", m.hud_on);
    text_button(StripButton::Fit, l.fit, m.one_to_one ? L"1:1" : L"FIT", m.one_to_one);

    // Caption buttons: hover/pressed fill bg2 (close: err), glyph textMid -> textHi.
    for (const StripButton b : {StripButton::Minimize, StripButton::Maximize, StripButton::Close}) {
        const D2D1_RECT_F& r = b == StripButton::Minimize ? l.minimize : b == StripButton::Maximize ? l.maximize : l.close;
        const bool hot = m.hover == b || m.pressed == b;
        const std::uint32_t bg = hot ? (b == StripButton::Close ? color::err : color::bg2) : color::bg0;
        if (hot) {
            p.fill(r, bg);
        }
        draw_caption_glyph(p, b, r, m.maximized, hot ? color::text_hi : color::text_mid, bg, dpi);
    }

    // Bottom hairline last, so hovered buttons never cover it.
    p.fill({l.strip.left, l.strip.bottom - hair, l.strip.right, l.strip.bottom}, color::line);
}

}  // namespace kvm::viewer
