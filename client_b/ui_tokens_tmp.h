// Design tokens from docs/DESIGN.md section 2 (+ the viewer numbers from section 4).
//
// TEMPORARY copy in Sandro's files: the real home is common/include/kvm/ui/tokens.h (Luka).
// Once Luka's version lands, switch the includes to it and delete this file. Every color/size
// used by the viewer lives here; drawing code must not contain magic numbers.
#pragma once

#include <windows.h>

#include <cmath>
#include <cstdint>

namespace kvm::ui {

// --- 2.1 Units: every size is in dp (1/96 inch); D2D targets run at 96 dpi, so px() = pixels.
inline int px(float dp, UINT dpi) {
    return static_cast<int>(std::lround(dp * static_cast<float>(dpi) / 96.0f));
}
inline int hairline(UINT dpi) {
    return px(1, dpi) > 1 ? px(1, dpi) : 1;
}

// --- 2.2 Color (0xRRGGBB, sRGB).
namespace color {
inline constexpr std::uint32_t bg0 = 0x0B0B0C;
inline constexpr std::uint32_t bg1 = 0x141416;
inline constexpr std::uint32_t bg2 = 0x1C1C1F;
inline constexpr std::uint32_t line = 0x2A2A2E;
inline constexpr std::uint32_t line_strong = 0x3A3A40;
inline constexpr std::uint32_t text_hi = 0xEDEDED;
inline constexpr std::uint32_t text_mid = 0xB4B4BA;
inline constexpr std::uint32_t text_lo = 0x8A8A92;
inline constexpr std::uint32_t accent = 0xFF5A1F;
inline constexpr std::uint32_t on_accent = 0x0B0B0C;
inline constexpr std::uint32_t ok = 0x3DDC84;
inline constexpr std::uint32_t warn = 0xFFC21A;
inline constexpr std::uint32_t err = 0xFF4D4D;
}  // namespace color

// --- 2.3 Typography.
enum class TextStyle { Label, Body, Title, Readout, Display };

struct TextStyleSpec {
    float size_dp;
    float line_height_dp;
    bool bold;
    bool uppercase;
    float letter_spacing_em;
};

inline constexpr TextStyleSpec text_style_spec(TextStyle s) {
    switch (s) {
    case TextStyle::Label:
        return {11, 14, false, true, 0.06f};
    case TextStyle::Body:
        return {13, 18, false, false, 0};
    case TextStyle::Title:
        return {15, 20, false, false, 0};
    case TextStyle::Readout:
        return {13, 18, true, false, 0};
    case TextStyle::Display:
        return {28, 32, true, true, 0.02f};
    }
    return {13, 18, false, false, 0};
}

inline constexpr wchar_t kFontFamily[] = L"JetBrains Mono";
inline constexpr wchar_t kFallbackFontFamily[] = L"Consolas";
inline constexpr wchar_t kFontRegularFile[] = L"JetBrainsMono-Regular.ttf";
inline constexpr wchar_t kFontBoldFile[] = L"JetBrainsMono-Bold.ttf";

// --- 2.4 Spacing scale (dp). Nothing else is allowed.
namespace space {
inline constexpr float s4 = 4;
inline constexpr float s8 = 8;
inline constexpr float s12 = 12;
inline constexpr float s16 = 16;
inline constexpr float s24 = 24;
inline constexpr float s32 = 32;
inline constexpr float s48 = 48;
}  // namespace space

// --- 2.5 Copy.
// App name (docs/DESIGN.md section 9): Sarke, Georgian სარკე, "mirror". Label styles uppercase it.
inline constexpr wchar_t kAppName[] = L"Sarke";        // window, tray, notifications, labels
inline constexpr wchar_t kAppNameIdle[] = L"sarke";    // viewer strip title when nothing streams
inline constexpr wchar_t kSeparator[] = L" · ";  // " · "
inline constexpr wchar_t kTimes[] = L"×";        // "×"
inline constexpr wchar_t kUnknown[] = L"—";      // "—"

// --- 9. App logo (Sarke concept C, "ს caret"): pixel Georgian ს + accent block caret, 16x16 grid.
namespace logo {
inline constexpr int grid = 16;
inline constexpr std::uint32_t background = color::bg0;
struct Cell {
    int x, y, w, h;  // in grid units
    std::uint32_t rgb;
};
inline constexpr Cell cells[] = {
    {2, 3, 2, 8, color::text_hi},   // ს tall left stem
    {3, 11, 6, 2, color::text_hi},  // ს bowl (open corners keep it round)
    {8, 6, 2, 5, color::text_hi},   // ს short right stem
    {11, 5, 3, 8, color::accent},   // block caret
};
}  // namespace logo

// --- 4. Viewer window (client B).
namespace viewer {
inline constexpr float initial_client_w = 960;
inline constexpr float initial_client_h = 600;
inline constexpr float min_client_w = 480;
inline constexpr float min_client_h = 320;
inline constexpr float stream_fit_fraction = 0.8f;  // of B's work area on stream_start

// 4.2 Title strip.
inline constexpr float strip_h = 32;
inline constexpr float resize_border = 6;
inline constexpr float status_square = 8;
inline constexpr float status_square_x = 12;
inline constexpr float state_word_x = 28;
inline constexpr float caption_button_w = 46;
inline constexpr float text_button_w = 40;
inline constexpr float caption_glyph = 10;
inline constexpr float restore_offset = 2;

// 4.4 State block.
inline constexpr float state_block_x = 48;
inline constexpr float caret_w = 12;
inline constexpr float caret_h = 28;
inline constexpr float footer_bottom_gap = 24;
inline constexpr float stalled_overlay_alpha = 0.6f;
inline constexpr int stall_after_ms = 1000;

// 4.5 HUD.
inline constexpr float hud_margin = 12;
inline constexpr float hud_w = 176;
inline constexpr float hud_pad_v = 10;
inline constexpr float hud_pad_h = 12;
inline constexpr float hud_row_h = 18;
inline constexpr float hud_alpha = 0.88f;
inline constexpr int hud_update_ms = 250;

inline constexpr int rtt_warn_ms = 20;
inline constexpr int rtt_err_ms = 50;
inline constexpr int fps_warn = 24;
inline constexpr int fps_err = 12;
inline constexpr int lat_warn_ms = 80;
inline constexpr int lat_err_ms = 150;
}  // namespace viewer

}  // namespace kvm::ui
