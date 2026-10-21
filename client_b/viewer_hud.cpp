// SPDX-License-Identifier: Apache-2.0

#include "viewer_hud.h"

#include <cmath>
#include <format>
#include <string>

namespace kvm::viewer {

namespace {

namespace vt = ui::viewer;
using ui::pal;
using ui::px;
using ui::TextStyle;

constexpr int kRows = 11;

struct Row {
    const wchar_t* label;
    std::wstring value;          // "—" when unknown
    const wchar_t* unit;         // nullptr = no unit
    std::uint32_t value_color;
};

std::uint32_t above(std::optional<int> v, int warn, int err) {
    if (!v) return pal().ink;
    if (*v > err) return pal().err;
    if (*v > warn) return pal().warn;
    return pal().ink;
}

std::uint32_t below(std::optional<int> v, int warn, int err) {
    if (!v) return pal().ink;
    if (*v < err) return pal().err;
    if (*v < warn) return pal().warn;
    return pal().ink;
}

std::wstring int_or_unknown(std::optional<int> v) {
    return v ? std::to_wstring(*v) : std::wstring(ui::kUnknown);
}

std::wstring tenths_or_unknown(std::optional<double> v) {
    return v ? std::format(L"{:.1f}", *v) : std::wstring(ui::kUnknown);
}

std::wstring count_or_unknown(std::optional<std::uint64_t> v) {
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
    const ui::Palette& c = pal();
    p.round(r, static_cast<float>(px(ui::radius::sm, dpi)), c.tile, ui::alpha::hud);

    const std::wstring res =
        v.width && v.height ? std::format(L"{}{}{}", *v.width, ui::kTimes, *v.height) : std::wstring(ui::kUnknown);
    // The website's feature list: "RTT, FPS, latency, bitrate, resolution and dropped frames"; decode,
    // present and repeats for the measurements in docs/PERF.md (roadmap P2.1).
    const Row rows[kRows] = {
        {L"RTT", int_or_unknown(v.rtt_ms), L"ms", above(v.rtt_ms, vt::rtt_warn_ms, vt::rtt_err_ms)},
        {L"FPS", int_or_unknown(v.fps), nullptr, below(v.fps, vt::fps_warn, vt::fps_err)},
        {L"Latency p50", int_or_unknown(v.latency_ms), L"ms", above(v.latency_ms, vt::lat_warn_ms, vt::lat_err_ms)},
        {L"Latency p95", int_or_unknown(v.latency_p95_ms), L"ms",
         above(v.latency_p95_ms, vt::lat_warn_ms, vt::lat_err_ms)},
        {L"Decode", tenths_or_unknown(v.decode_ms), L"ms", c.ink},
        {L"Present", tenths_or_unknown(v.present_ms), L"ms", c.ink},
        {L"Bitrate", tenths_or_unknown(v.mbps), L"Mbps", c.ink},
        {L"Resolution", res, nullptr, c.ink},
        {L"Quality", int_or_unknown(v.jpeg_quality), nullptr, c.ink},
        {L"Dropped", count_or_unknown(v.dropped), nullptr, c.ink},
        {L"Repeated", count_or_unknown(v.repeated), nullptr, c.ink},
    };

    const auto pad_h = static_cast<float>(px(vt::hud_pad_h, dpi));
    const auto row_h = static_cast<float>(px(vt::hud_row_h, dpi));
    const float left = r.left + pad_h;
    const float right = r.right - pad_h;
    const auto unit_gap = static_cast<float>(px(ui::space::s4, dpi));
    const float caption_h = p.styles().metrics(TextStyle::Caption).line_px;
    const float value_h = p.styles().metrics(TextStyle::MonoStrong).line_px;
    float row_top = r.top + static_cast<float>(px(vt::hud_pad_v, dpi));

    for (const Row& row : rows) {
        const float label_top = std::floor(row_top + (row_h - caption_h) / 2);
        const float value_top = std::floor(row_top + (row_h - value_h) / 2);
        p.text(TextStyle::Caption, row.label, left, label_top, right - left, c.ink3);

        float value_right = right;
        if (row.unit && row.value != ui::kUnknown) {
            value_right = p.text_right(TextStyle::Caption, row.unit, right, label_top, c.ink3) - unit_gap;
        }
        p.text_right(TextStyle::MonoStrong, row.value, value_right, value_top, row.value_color);
        row_top += row_h;
    }
}

}  // namespace kvm::viewer
