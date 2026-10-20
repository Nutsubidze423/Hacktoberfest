// SPDX-License-Identifier: Apache-2.0

// Direct2D / DirectWrite helpers for Sarke's surfaces (docs/DESIGN.md section 2).
//
// Everything here runs on the UI thread. Render targets use 96 dpi, so 1 D2D unit = 1 pixel and
// all sizes go through px() from ui_tokens.h. Colours come from pal() (dark / light theme).
#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "ui_tokens.h"

namespace kvm::ui {

using Microsoft::WRL::ComPtr;

D2D1_COLOR_F to_color(std::uint32_t rgb, float alpha = 1.0f);

// Process-wide D2D and DirectWrite factories plus the bundled fonts (Google Sans, Geist Mono).
// Falls back to Segoe UI Variable / Cascadia Mono (logged once) when the font files are missing.
class Graphics {
public:
    // Throws std::runtime_error if the factories cannot be created.
    static Graphics& get();

    ID2D1Factory* d2d() const { return d2d_.Get(); }
    IDWriteFactory* dwrite() const { return dwrite_.Get(); }
    // The collection and family for a font role; collection nullptr = the system collection.
    IDWriteFontCollection* fonts(FontRole role) const;
    const wchar_t* family(FontRole role) const;

private:
    Graphics();
    void load_fonts();

    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDWriteFontCollection> bundled_;
    bool sans_bundled_ = false;
    bool mono_bundled_ = false;
    const wchar_t* sans_ = kSansFallback;
    const wchar_t* mono_ = kMonoFallback;
};

// The text styles at one DPI. Call create() again after WM_DPICHANGED.
class TextStyles {
public:
    void create(UINT dpi);

    struct Metrics {
        float line_px = 0;     // line box height
        float cap_top_px = 0;  // from the line top to the top of capital letters
    };
    Metrics metrics(TextStyle style) const { return entries_[index(style)].metrics; }

    // Single-line layout with ellipsis trimming at max_width, letter spacing applied.
    ComPtr<IDWriteTextLayout> layout(TextStyle style, std::wstring_view text, float max_width) const;
    // Advance width of the text (without the trailing letter spacing of the last glyph).
    float measure(TextStyle style, std::wstring_view text) const;
    // The format, e.g. for multi-line text drawn with DrawText.
    IDWriteTextFormat* format(TextStyle style) const { return entries_[index(style)].format.Get(); }

private:
    struct Entry {
        ComPtr<IDWriteTextFormat> format;
        float spacing_px = 0;
        Metrics metrics;
    };
    static std::size_t index(TextStyle s) { return static_cast<std::size_t>(s); }

    std::array<Entry, kTextStyleCount> entries_;
};

// Drawing on one render target with one reusable brush. Rects are expected at integer pixels.
class Painter {
public:
    Painter(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const TextStyles& text)
        : rt_(rt), brush_(brush), text_(text) {}

    void fill(const D2D1_RECT_F& r, std::uint32_t rgb, float alpha = 1.0f);
    // Border of `thickness` pixels drawn inside `r` as four filled rects (never blurred).
    void frame(const D2D1_RECT_F& r, float thickness, std::uint32_t rgb, float alpha = 1.0f);
    // Rounded rectangle (antialiased); radius is clamped to half the shorter side.
    void round(const D2D1_RECT_F& r, float radius, std::uint32_t rgb, float alpha = 1.0f);
    // Rounded outline of `width` pixels drawn inside `r`.
    void round_stroke(const D2D1_RECT_F& r, float radius, float width, std::uint32_t rgb, float alpha = 1.0f);
    // Fully rounded ends (buttons, segmented controls, status pills).
    void pill(const D2D1_RECT_F& r, std::uint32_t rgb, float alpha = 1.0f);
    void circle(D2D1_POINT_2F center, float radius, std::uint32_t rgb, float alpha = 1.0f);
    // Antialiased stroke, for non axis-aligned lines (e.g. the close glyph).
    void diagonal(D2D1_POINT_2F a, D2D1_POINT_2F b, float width, std::uint32_t rgb, float alpha = 1.0f);

    // The website's background: two soft radial halos (top centre and bottom right) over `r`.
    void halo(const D2D1_RECT_F& r);
    // The Sarke mark in a square `r` (docs/DESIGN.md 9), antialiased.
    void logo(const D2D1_RECT_F& r);

    // Draws one line of text with its line box top at `top`. Returns the drawn advance width.
    float text(TextStyle style, std::wstring_view s, float x, float top, float max_width, std::uint32_t rgb,
               float alpha = 1.0f);
    // Same, right-aligned so the text ends at `right`. Returns the text's left x.
    float text_right(TextStyle style, std::wstring_view s, float right, float top, std::uint32_t rgb);
    // One line centred in `r` (both ways). Returns the text's left x.
    float text_center(TextStyle style, std::wstring_view s, const D2D1_RECT_F& r, std::uint32_t rgb, float alpha = 1.0f);

    const TextStyles& styles() const { return text_; }
    ID2D1RenderTarget* rt() const { return rt_; }

private:
    ID2D1RenderTarget* rt_;
    ID2D1SolidColorBrush* brush_;
    const TextStyles& text_;
};

// A premultiplied 32-bpp canvas drawn with Direct2D, for surfaces with real transparency: icons
// (antialiased rounded tile) and WS_EX_LAYERED popups with rounded corners (notification card,
// Identify). Text on it is grayscale-antialiased (ClearType needs an opaque background).
class LayeredSurface {
public:
    LayeredSurface() = default;
    ~LayeredSurface();
    LayeredSurface(const LayeredSurface&) = delete;
    LayeredSurface& operator=(const LayeredSurface&) = delete;

    // Starts a frame of w x h pixels cleared to transparent. nullptr if D2D / GDI failed.
    ID2D1RenderTarget* begin(int w, int h);
    ID2D1SolidColorBrush* brush() const { return brush_.Get(); }
    bool end();
    // Puts the last frame on a WS_EX_LAYERED window at (x, y) (screen pixels), faded by `alpha`.
    void present(HWND hwnd, int x, int y, BYTE alpha) const;
    // The last frame, premultiplied BGRA, top-down, w * h pixels.
    const std::uint32_t* pixels() const { return static_cast<const std::uint32_t*>(bits_); }
    int width() const { return w_; }
    int height() const { return h_; }

private:
    void release();

    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_ = nullptr;
    void* bits_ = nullptr;
    int w_ = 0;
    int h_ = 0;
    ComPtr<ID2D1DCRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
};

// Frameless Sarke windows: Windows 11 rounded corners, no 1 px system border, dark / light
// non-client parts (shadow, Snap layouts) matching the theme. Call after creation and on theme change.
void style_window(HWND hwnd, bool round_corners = true);

// True if this WM_SETTINGCHANGE is Windows' app mode switching (lParam "ImmersiveColorSet").
bool is_theme_change(UINT msg, LPARAM lp);

}  // namespace kvm::ui
