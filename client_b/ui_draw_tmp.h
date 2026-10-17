// Direct2D / DirectWrite helpers for styled surfaces (docs/DESIGN.md section 2).
//
// TEMPORARY copy in Sandro's files: the real home is common/include/kvm/ui/draw.h +
// common/src/ui_draw.cpp (Luka). Replace once that lands.
//
// Everything here runs on the UI thread. Render targets use 96 dpi, so 1 D2D unit = 1 pixel and
// all sizes go through px() from ui_tokens_tmp.h.
#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "ui_tokens_tmp.h"

namespace kvm::ui {

using Microsoft::WRL::ComPtr;

D2D1_COLOR_F to_color(std::uint32_t rgb, float alpha = 1.0f);

// Process-wide D2D and DirectWrite factories plus the bundled JetBrains Mono collection.
// Falls back to Consolas (logged once) when the font files are not found.
class Graphics {
public:
    // Throws std::runtime_error if the factories cannot be created.
    static Graphics& get();

    ID2D1Factory* d2d() const { return d2d_.Get(); }
    IDWriteFactory* dwrite() const { return dwrite_.Get(); }
    IDWriteFontCollection* fonts() const { return fonts_.Get(); }  // nullptr = system collection
    const wchar_t* family() const { return family_; }

private:
    Graphics();
    void load_fonts();

    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDWriteFontCollection> fonts_;
    const wchar_t* family_ = kFallbackFontFamily;
};

// The five text styles at one DPI. Call create() again after WM_DPICHANGED.
class TextStyles {
public:
    void create(UINT dpi);

    struct Metrics {
        float line_px = 0;     // line box height
        float cap_top_px = 0;  // from the line top to the top of capital letters
    };
    Metrics metrics(TextStyle style) const { return entries_[index(style)].metrics; }

    // Single-line layout with ellipsis trimming at max_width, uppercase/letter spacing applied.
    ComPtr<IDWriteTextLayout> layout(TextStyle style, std::wstring_view text, float max_width) const;
    // Advance width of the text (without the trailing letter spacing of the last glyph).
    float measure(TextStyle style, std::wstring_view text) const;

private:
    struct Entry {
        ComPtr<IDWriteTextFormat> format;
        float spacing_px = 0;
        bool uppercase = false;
        Metrics metrics;
    };
    static std::size_t index(TextStyle s) { return static_cast<std::size_t>(s); }

    std::array<Entry, 5> entries_;
};

// Drawing on one render target with one reusable brush. Rects are expected at integer pixels.
class Painter {
public:
    Painter(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const TextStyles& text)
        : rt_(rt), brush_(brush), text_(text) {}

    void fill(const D2D1_RECT_F& r, std::uint32_t rgb, float alpha = 1.0f);
    // Border of `thickness` pixels drawn inside `r` as four filled rects (never blurred).
    void frame(const D2D1_RECT_F& r, float thickness, std::uint32_t rgb, float alpha = 1.0f);
    // Antialiased stroke — only for non axis-aligned lines (e.g. the close glyph).
    void diagonal(D2D1_POINT_2F a, D2D1_POINT_2F b, float width, std::uint32_t rgb);

    // Draws one line of text with its line box top at `top`. Returns the drawn advance width.
    float text(TextStyle style, std::wstring_view s, float x, float top, float max_width, std::uint32_t rgb);
    // Same, right-aligned so the text ends at `right`. Returns the text's left x.
    float text_right(TextStyle style, std::wstring_view s, float right, float top, std::uint32_t rgb);

    const TextStyles& styles() const { return text_; }
    ID2D1RenderTarget* rt() const { return rt_; }

private:
    ID2D1RenderTarget* rt_;
    ID2D1SolidColorBrush* brush_;
    const TextStyles& text_;
};

}  // namespace kvm::ui
