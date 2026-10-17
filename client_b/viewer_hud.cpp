#include "viewer_hud.h"

#include <cmath>
#include <format>
#include <string>

namespace kvm::viewer {

namespace {

namespace vt = ui::viewer;
namespace color = ui::color;
using ui::px;
using ui::TextStyle;

constexpr int kRows = 7;

struct Row {
    const wchar_t* label;
    std::wstring value;          // "—" when unknown
    const wchar_t* unit;         // nullptr = no unit
    std::uint32_t value_color;
};

std::uint32_t above(std::optional<int> v, int warn, int err) {
    if (!v) return color::text_hi;
    if (*v > err) return color::err;
    if (*v > warn) return color::warn;
    return color::text_hi;
}

std::uint32_t below(std::optional<int> v, int warn, int err) {
    if (!v) return color::text_hi;
    if (*v < err) return color::err;
    if (*v < warn) return color::warn;
    return color::text_hi;
}

std::wstring int_or_unknown(std::optional<int> v) {
    return v ? std::to_wstring(*v) : std::wstring(ui::kUnknown);
}

}  // namespace

D2D1_RECT_F hud_rect(float client_w, float content_top, UINT dpi) {
    const auto margin = static_cast<float>(px(vt::hud_margin, dpi));
    const auto w = static_cast<float>(px(vt::hud_w, dpi));
    const auto h = static_cast<float>(2 * px(vt::hud_pad_v, dpi) + kRows * px(vt::hud_row_h, dpi));
    const float right = client_w - margin;
    const float top = content_top + margin;
    return {right - w, top, right, top + h};
}

void draw_hud(ui::Painter& p, const D2D1_RECT_F& r, const HudValues& v, UINT dpi) {
    p.fill(r, color::bg0, vt::hud_alpha);
    p.frame(r, static_cast<float>(ui::hairline(dpi)), color::line);

    const std::wstring res = v.width && v.height ? std::format(L"{}{}{}", *v.width, ui::kTimes, *v.height)
                                                 : std::wstring(ui::kUnknown);
    const Row rows[kRows] = {
        {L"RTT", int_or_unknown(v.rtt_ms), L"ms", above(v.rtt_ms, vt::rtt_warn_ms, vt::rtt_err_ms)},
        {L"FPS", int_or_unknown(v.fps), nullptr, below(v.fps, vt::fps_warn, vt::fps_err)},
        {L"LAT", int_or_unknown(v.latency_ms), L"ms", above(v.latency_ms, vt::lat_warn_ms, vt::lat_err_ms)},
        {L"MBPS", v.mbps ? std::format(L"{:.1f}", *v.mbps) : std::wstring(ui::kUnknown), nullptr, color::text_hi},
        {L"RES", res, nullptr, color::text_hi},
        {L"Q", int_or_unknown(v.jpeg_quality), nullptr, color::text_hi},
        {L"DROP", v.dropped ? std::to_wstring(*v.dropped) : std::wstring(ui::kUnknown), nullptr, color::text_hi},
    };

    const auto pad_h = static_cast<float>(px(vt::hud_pad_h, dpi));
    const auto row_h = static_cast<float>(px(vt::hud_row_h, dpi));
    const float left = r.left + pad_h;
    const float right = r.right - pad_h;
    const float space_w = std::round(p.styles().measure(TextStyle::Label, L" "));
    float row_top = r.top + static_cast<float>(px(vt::hud_pad_v, dpi));

    for (const Row& row : rows) {
        const float label_top = std::floor(row_top + (row_h - p.styles().metrics(TextStyle::Label).line_px) / 2);
        const float readout_top = std::floor(row_top + (row_h - p.styles().metrics(TextStyle::Readout).line_px) / 2);
        p.text(TextStyle::Label, row.label, left, label_top, right - left, color::text_lo);

        float value_right = right;
        if (row.unit && row.value != ui::kUnknown) {
            value_right = p.text_right(TextStyle::Label, row.unit, right, label_top, color::text_lo) - space_w;
        }
        p.text_right(TextStyle::Readout, row.value, value_right, readout_top, row.value_color);
        row_top += row_h;
    }
}

}  // namespace kvm::viewer
