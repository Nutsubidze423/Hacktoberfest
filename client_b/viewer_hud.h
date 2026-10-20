// HUD readout overlay of the viewer (docs/DESIGN.md 4.5). Unknown values are drawn as "—".
#pragma once

#include <d2d1.h>

#include <cstdint>
#include <optional>

#include "ui_draw.h"

namespace kvm::viewer {

struct HudValues {
    std::optional<int> rtt_ms;
    std::optional<int> fps;
    std::optional<int> latency_ms;
    std::optional<double> mbps;
    std::optional<int> width;
    std::optional<int> height;
    std::optional<int> jpeg_quality;
    std::optional<std::uint64_t> dropped;
};

// Top-right of the content area: top = content_top + 12 dp, right = client_w - 12 dp, fixed size.
D2D1_RECT_F hud_rect(float client_w, float content_top, UINT dpi);

void draw_hud(ui::Painter& p, const D2D1_RECT_F& rect, const HudValues& values, UINT dpi);

}  // namespace kvm::viewer
