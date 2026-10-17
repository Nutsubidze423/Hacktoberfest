// Design tokens (docs/DESIGN.md section 2): the same system as the Sarke website
// (sarke-nine.vercel.app, D:\sarke-web\src\app\globals.css). Apple-like restraint: a neutral
// black / white / grey base, one blue for actions and links, solid rounded tiles, no borders or
// glows. Dark and light themes; the app follows Windows' app mode like the site follows the system.
//
// TEMPORARY copy in Sandro's files: the real home is common/include/kvm/ui/tokens.h (Luka).
// Every color/size used by Sarke's surfaces lives here; drawing code must not contain magic numbers.
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

// --- 2.2 Color (0xRRGGBB, sRGB). Lines are ink at an alpha (alpha::line), as on the website.
enum class Theme { Dark, Light };

struct Palette {
    std::uint32_t bg;        // window background            (--bg)
    std::uint32_t bg_alt;    // second background            (--bg-alt)
    std::uint32_t tile;      // raised tiles, cards, panels  (--bg-raise)
    std::uint32_t fill;      // fills inside tiles: buttons, monitors, segmented track (--bg-high)
    std::uint32_t fill_hi;   // hover inside tiles
    std::uint32_t seg_on;    // the selected segment of a segmented control
    std::uint32_t ink;       // primary text                 (--ink)
    std::uint32_t ink2;      // secondary text               (--ink-2)
    std::uint32_t ink3;      // tertiary text, captions      (--ink-3)
    std::uint32_t accent;    // button fill                  (--accent)
    std::uint32_t accent_hover;
    std::uint32_t accent_ink;  // links and accent text      (--accent-ink)
    std::uint32_t on_accent;
    std::uint32_t ok;
    std::uint32_t warn;
    std::uint32_t err;
    std::uint32_t halo1;     // the background halos (radial, alpha below)
    std::uint32_t halo2;
};

inline constexpr Palette kDark{
    .bg = 0x050506, .bg_alt = 0x0B0B0D, .tile = 0x1D1D1F, .fill = 0x2C2C2E, .fill_hi = 0x3A3A3C, .seg_on = 0x545458,
    .ink = 0xF5F5F7, .ink2 = 0xA1A1A6, .ink3 = 0x86868B,
    .accent = 0x0071E3, .accent_hover = 0x0077ED, .accent_ink = 0x2997FF, .on_accent = 0xFFFFFF,
    .ok = 0x30D158, .warn = 0xFF9F0A, .err = 0xFF453A, .halo1 = 0x6276A4, .halo2 = 0x485474,
};
inline constexpr Palette kLight{
    .bg = 0xFBFBFD, .bg_alt = 0xF5F5F7, .tile = 0xF5F5F7, .fill = 0xE8E8ED, .fill_hi = 0xDCDCE1, .seg_on = 0xFFFFFF,
    .ink = 0x1D1D1F, .ink2 = 0x424245, .ink3 = 0x6E6E73,
    .accent = 0x0071E3, .accent_hover = 0x0077ED, .accent_ink = 0x0066CC, .on_accent = 0xFFFFFF,
    .ok = 0x248A3D, .warn = 0xB25000, .err = 0xD70015, .halo1 = 0x7896D2, .halo2 = 0x96A0C8,
};

// Windows 11's own caption close button (hover fill, glyph), so ours feels native.
inline constexpr std::uint32_t kCloseHover = 0xC42B1C;
inline constexpr std::uint32_t kCloseGlyph = 0xFFFFFF;

namespace alpha {
inline constexpr float line = 0.10f;         // hairlines, separators (ink)
inline constexpr float line_strong = 0.16f;  // drop zones, outlines (ink)
inline constexpr float halo1 = 0.17f;
inline constexpr float halo2 = 0.11f;
inline constexpr float scrim = 0.62f;        // bg over a stalled picture
inline constexpr float hud = 0.90f;          // HUD tile over the picture
inline constexpr float disabled = 0.38f;
}  // namespace alpha

// Windows' app mode (Settings > Personalization > Colors > Choose your app mode), unless the
// environment variable SARKE_THEME is "light" or "dark".
Theme system_theme();
// The theme every surface draws with. refresh_theme() re-reads Windows; call it on
// WM_SETTINGCHANGE ("ImmersiveColorSet") and repaint. Returns true if it changed.
Theme current_theme();
bool refresh_theme();
const Palette& pal();

// --- 2.3 Typography: Google Sans for text, Geist Mono for numbers / addresses / code (the site's
// --font and --font-code). Sentence case everywhere; tight tracking on big type.
enum class TextStyle { Caption, Body, BodyStrong, Title, Headline, Display, Mono, MonoStrong };
inline constexpr int kTextStyleCount = 8;

enum class FontRole { Sans, Mono };

struct TextStyleSpec {
    float size_dp;
    float line_height_dp;
    int weight;               // 400 / 500 / 600 / 700
    FontRole font;
    float letter_spacing_em;  // negative = tighter
};

inline constexpr TextStyleSpec text_style_spec(TextStyle s) {
    switch (s) {
    case TextStyle::Caption:    return {12, 16, 400, FontRole::Sans, 0};
    case TextStyle::Body:       return {14, 20, 400, FontRole::Sans, -0.005f};
    case TextStyle::BodyStrong: return {14, 20, 500, FontRole::Sans, -0.005f};
    case TextStyle::Title:      return {17, 22, 600, FontRole::Sans, -0.01f};
    case TextStyle::Headline:   return {28, 32, 700, FontRole::Sans, -0.02f};
    case TextStyle::Display:    return {44, 48, 700, FontRole::Sans, -0.03f};
    case TextStyle::Mono:       return {12, 16, 400, FontRole::Mono, 0};
    case TextStyle::MonoStrong: return {13, 18, 500, FontRole::Mono, 0};
    }
    return {14, 20, 400, FontRole::Sans, 0};
}

// Bundled in assets\fonts (SIL OFL 1.1); system fonts if they are missing.
inline constexpr wchar_t kSansFamily[] = L"Google Sans";
inline constexpr wchar_t kMonoFamily[] = L"Geist Mono";
inline constexpr wchar_t kSansFallback[] = L"Segoe UI Variable Text";
inline constexpr wchar_t kSansFallback2[] = L"Segoe UI";
inline constexpr wchar_t kMonoFallback[] = L"Cascadia Mono";
inline constexpr wchar_t kMonoFallback2[] = L"Consolas";
inline constexpr const wchar_t* kFontFiles[] = {
    L"GoogleSans-Regular.ttf", L"GoogleSans-Medium.ttf", L"GoogleSans-SemiBold.ttf", L"GoogleSans-Bold.ttf",
    L"GeistMono-Regular.ttf",  L"GeistMono-Medium.ttf",  L"GeistMono-SemiBold.ttf",
};

// --- 2.4 Spacing scale (dp) and corner radii (the site's --s-* and --r-*).
namespace space {
inline constexpr float s4 = 4;
inline constexpr float s8 = 8;
inline constexpr float s12 = 12;
inline constexpr float s16 = 16;
inline constexpr float s24 = 24;
inline constexpr float s32 = 32;
inline constexpr float s48 = 48;
}  // namespace space

namespace radius {
inline constexpr float xs = 6;   // monitors on the map, small chips
inline constexpr float sm = 12;  // HUD, menus, segmented segments
inline constexpr float md = 18;  // cards, notification
inline constexpr float lg = 28;  // big tiles (map, panels)
// Buttons and segmented controls are pills: radius = height / 2.
}  // namespace radius

// --- 2.5 Copy.
// App name (docs/DESIGN.md section 9): Sarke, Georgian სარკე, "mirror".
inline constexpr wchar_t kAppName[] = L"Sarke";
inline constexpr wchar_t kAppNameIdle[] = L"Sarke";  // viewer title when nothing streams
inline constexpr wchar_t kSeparator[] = L" · ";
inline constexpr wchar_t kTimes[] = L"×";
inline constexpr wchar_t kUnknown[] = L"—";
inline constexpr wchar_t kChevron[] = L"›";

// --- 9. The Sarke mark (the website's src/app/icon.tsx): one desktop mirrored into the next.
// A dark rounded tile; a solid light rounded square top-left and an outlined one bottom-right
// overlapping it. Fractions of the tile size; the same in both themes.
namespace logo {
inline constexpr std::uint32_t tile = 0x050506;
inline constexpr std::uint32_t mark = 0xF5F5F7;
inline constexpr float tile_radius = 0.22f;
inline constexpr float square = 0.44f;
inline constexpr float square_radius = 0.10f;
inline constexpr float stroke = 0.07f;
inline constexpr float inset = 0.17f;
}  // namespace logo

// --- 4. Viewer window.
namespace viewer {
inline constexpr float initial_client_w = 960;
inline constexpr float initial_client_h = 600;
inline constexpr float min_client_w = 480;
inline constexpr float min_client_h = 320;
inline constexpr float stream_fit_fraction = 0.8f;  // of the work area on stream_start

// 4.2 Title strip.
inline constexpr float strip_h = 44;
inline constexpr float resize_border = 6;
inline constexpr float logo = 20;
inline constexpr float logo_x = 14;
inline constexpr float pill_h = 26;        // status pill and HUD / FIT toggles
inline constexpr float pill_pad = 10;
inline constexpr float status_dot = 7;
inline constexpr float caption_button_w = 46;
inline constexpr float toggle_gap = 4;
inline constexpr float caption_glyph = 10;
inline constexpr float restore_offset = 2;

// 4.4 State screens (centred, like the website's hero).
inline constexpr float footer_bottom_gap = 24;
inline constexpr float keycap_h = 26;
inline constexpr float keycap_pad = 8;
inline constexpr int stall_after_ms = 1000;

// 4.5 HUD.
inline constexpr float hud_margin = 12;
inline constexpr float hud_w = 188;
inline constexpr float hud_pad_v = 10;
inline constexpr float hud_pad_h = 14;
inline constexpr float hud_row_h = 22;
inline constexpr int hud_update_ms = 250;

inline constexpr int rtt_warn_ms = 20;
inline constexpr int rtt_err_ms = 50;
inline constexpr int fps_warn = 24;
inline constexpr int fps_err = 12;
inline constexpr int lat_warn_ms = 80;
inline constexpr int lat_err_ms = 150;
}  // namespace viewer

}  // namespace kvm::ui
