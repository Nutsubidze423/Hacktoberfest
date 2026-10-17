// Custom title strip of the viewer window (docs/DESIGN.md 4.2): layout, hit testing, drawing.
#pragma once

#include <d2d1.h>

#include <cstdint>
#include <string_view>

#include "ui_draw_tmp.h"

namespace kvm::viewer {

enum class StripButton { None, Hud, Fit, Minimize, Maximize, Close };

// Everything in client pixels; the strip sits at the top of the client area.
struct StripLayout {
    D2D1_RECT_F strip{};
    D2D1_RECT_F hud{};
    D2D1_RECT_F fit{};
    D2D1_RECT_F minimize{};
    D2D1_RECT_F maximize{};
    D2D1_RECT_F close{};
};

StripLayout layout_strip(float client_w, UINT dpi);
StripButton hit_test(const StripLayout& layout, float x, float y);

struct StripModel {
    std::uint32_t status_color = 0;
    bool status_visible = true;  // false during the "off" phase of a blink
    std::wstring_view state_word;
    std::wstring_view title;
    bool hud_on = true;
    bool one_to_one = false;
    bool maximized = false;
    StripButton hover = StripButton::None;
    StripButton pressed = StripButton::None;
};

void draw_strip(ui::Painter& p, const StripLayout& layout, const StripModel& model, UINT dpi);

}  // namespace kvm::viewer
