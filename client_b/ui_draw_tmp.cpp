#include "ui_draw_tmp.h"

#include <dwrite_3.h>

#include <cmath>
#include <filesystem>
#include <optional>
#include <stdexcept>

#include "kvm/log.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace kvm::ui {

namespace {

namespace fs = std::filesystem;

constexpr float kUnboundedWidth = 100000.0f;
constexpr int kFontSearchDepth = 5;  // exe dir + parents, e.g. build\x64-release\tools -> repo root

std::string utf8(const std::wstring& s) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring to_upper(std::wstring_view s) {
    std::wstring out(s);
    if (!out.empty()) {
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, s.data(), static_cast<int>(s.size()), out.data(),
                      static_cast<int>(out.size()), nullptr, nullptr, 0);
    }
    return out;
}

// assets\fonts next to the exe or in one of its parent directories (the repo copy during development).
std::optional<fs::path> find_font_dir() {
    std::wstring exe(MAX_PATH, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
    fs::path dir = fs::path(exe).parent_path();
    for (int i = 0; i < kFontSearchDepth; ++i) {
        const fs::path candidate = dir / L"assets" / L"fonts";
        std::error_code ec;
        if (fs::exists(candidate / kFontRegularFile, ec) && fs::exists(candidate / kFontBoldFile, ec)) {
            return candidate;
        }
        if (dir == dir.parent_path()) {
            break;
        }
        dir = dir.parent_path();
    }
    return std::nullopt;
}

ComPtr<IDWriteFont> find_font(IDWriteFactory* dwrite, IDWriteFontCollection* collection, const wchar_t* family,
                              DWRITE_FONT_WEIGHT weight) {
    ComPtr<IDWriteFontCollection> system;
    if (!collection) {
        dwrite->GetSystemFontCollection(&system);
        collection = system.Get();
    }
    UINT32 index = 0;
    BOOL exists = FALSE;
    ComPtr<IDWriteFontFamily> font_family;
    ComPtr<IDWriteFont> font;
    if (SUCCEEDED(collection->FindFamilyName(family, &index, &exists)) && exists &&
        SUCCEEDED(collection->GetFontFamily(index, &font_family))) {
        font_family->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font);
    }
    return font;
}

}  // namespace

D2D1_COLOR_F to_color(std::uint32_t rgb, float alpha) {
    return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                        static_cast<float>(rgb & 0xFF) / 255.0f, alpha);
}

// ---------------------------------------------------------------------------------------------
// Graphics

Graphics& Graphics::get() {
    static Graphics instance;
    return instance;
}

Graphics::Graphics() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) {
        throw std::runtime_error("D2D1CreateFactory failed");
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())))) {
        throw std::runtime_error("DWriteCreateFactory failed");
    }
    load_fonts();
}

void Graphics::load_fonts() {
    const auto dir = find_font_dir();
    if (!dir) {
        log::warn("ui: assets\\fonts\\{} not found, falling back to {}", utf8(kFontRegularFile),
                  utf8(kFallbackFontFamily));
        return;
    }

    ComPtr<IDWriteFactory5> factory5;
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(dwrite_.As(&factory5)) || FAILED(factory5->CreateFontSetBuilder(&builder))) {
        log::warn("ui: DirectWrite font sets unavailable, falling back to {}", utf8(kFallbackFontFamily));
        return;
    }
    for (const wchar_t* file : {kFontRegularFile, kFontBoldFile}) {
        ComPtr<IDWriteFontFile> font_file;
        if (FAILED(factory5->CreateFontFileReference((*dir / file).c_str(), nullptr, &font_file)) ||
            FAILED(builder->AddFontFile(font_file.Get()))) {
            log::warn("ui: cannot load {}, falling back to {}", utf8((*dir / file).wstring()), utf8(kFallbackFontFamily));
            return;
        }
    }
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> collection;
    if (FAILED(builder->CreateFontSet(&set)) || FAILED(factory5->CreateFontCollectionFromFontSet(set.Get(), &collection)) ||
        !find_font(dwrite_.Get(), collection.Get(), kFontFamily, DWRITE_FONT_WEIGHT_REGULAR)) {
        log::warn("ui: font collection from {} failed, falling back to {}", utf8(dir->wstring()),
                  utf8(kFallbackFontFamily));
        return;
    }
    fonts_ = collection;
    family_ = kFontFamily;
    log::info("ui: fonts loaded from {}", utf8(dir->wstring()));
}

// ---------------------------------------------------------------------------------------------
// TextStyles

void TextStyles::create(UINT dpi) {
    Graphics& g = Graphics::get();
    for (TextStyle style : {TextStyle::Label, TextStyle::Body, TextStyle::Title, TextStyle::Readout, TextStyle::Display}) {
        const TextStyleSpec spec = text_style_spec(style);
        const auto size = static_cast<float>(px(spec.size_dp, dpi));
        const auto line = static_cast<float>(px(spec.line_height_dp, dpi));
        const DWRITE_FONT_WEIGHT weight = spec.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_REGULAR;

        Entry& e = entries_[index(style)];
        e = Entry{};
        if (FAILED(g.dwrite()->CreateTextFormat(g.family(), g.fonts(), weight, DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &e.format))) {
            throw std::runtime_error("CreateTextFormat failed");
        }
        e.format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        g.dwrite()->CreateEllipsisTrimmingSign(e.format.Get(), &ellipsis);
        e.format->SetTrimming(&trimming, ellipsis.Get());

        // Center the font's ascent+descent in the line box and put the baseline on a whole pixel.
        float baseline = line * 0.8f;
        float cap_height = size * 0.7f;
        if (const auto font = find_font(g.dwrite(), g.fonts(), g.family(), weight)) {
            DWRITE_FONT_METRICS fm{};
            font->GetMetrics(&fm);
            const float k = size / static_cast<float>(fm.designUnitsPerEm);
            baseline = std::round((line - (fm.ascent + fm.descent) * k) / 2.0f + fm.ascent * k);
            cap_height = fm.capHeight * k;
        }
        e.format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line, baseline);

        e.spacing_px = spec.letter_spacing_em * size;
        e.uppercase = spec.uppercase;
        e.metrics = {.line_px = line, .cap_top_px = std::round(baseline - cap_height)};
    }
}

ComPtr<IDWriteTextLayout> TextStyles::layout(TextStyle style, std::wstring_view text, float max_width) const {
    const Entry& e = entries_[index(style)];
    const std::wstring s = e.uppercase ? to_upper(text) : std::wstring(text);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(Graphics::get().dwrite()->CreateTextLayout(s.data(), static_cast<UINT32>(s.size()), e.format.Get(),
                                                         max_width, e.metrics.line_px, &layout))) {
        return nullptr;
    }
    if (e.spacing_px > 0 && !s.empty()) {
        ComPtr<IDWriteTextLayout1> layout1;
        if (SUCCEEDED(layout.As(&layout1))) {
            layout1->SetCharacterSpacing(0, e.spacing_px, 0, {0, static_cast<UINT32>(s.size())});
        }
    }
    return layout;
}

float TextStyles::measure(TextStyle style, std::wstring_view text) const {
    if (text.empty()) {
        return 0;
    }
    const auto l = layout(style, text, kUnboundedWidth);
    DWRITE_TEXT_METRICS m{};
    if (!l || FAILED(l->GetMetrics(&m))) {
        return 0;
    }
    return m.widthIncludingTrailingWhitespace - entries_[index(style)].spacing_px;
}

// ---------------------------------------------------------------------------------------------
// Painter

void Painter::fill(const D2D1_RECT_F& r, std::uint32_t rgb, float alpha) {
    brush_->SetColor(to_color(rgb, alpha));
    rt_->FillRectangle(r, brush_);
}

void Painter::frame(const D2D1_RECT_F& r, float t, std::uint32_t rgb, float alpha) {
    fill({r.left, r.top, r.right, r.top + t}, rgb, alpha);
    fill({r.left, r.bottom - t, r.right, r.bottom}, rgb, alpha);
    fill({r.left, r.top + t, r.left + t, r.bottom - t}, rgb, alpha);
    fill({r.right - t, r.top + t, r.right, r.bottom - t}, rgb, alpha);
}

void Painter::diagonal(D2D1_POINT_2F a, D2D1_POINT_2F b, float width, std::uint32_t rgb) {
    brush_->SetColor(to_color(rgb));
    rt_->DrawLine(a, b, brush_, width);
}

float Painter::text(TextStyle style, std::wstring_view s, float x, float top, float max_width, std::uint32_t rgb) {
    if (s.empty() || max_width <= 0) {
        return 0;
    }
    const auto layout = text_.layout(style, s, max_width);
    if (!layout) {
        return 0;
    }
    brush_->SetColor(to_color(rgb));
    rt_->DrawTextLayout({std::round(x), std::round(top)}, layout.Get(), brush_);
    const float width = text_.measure(style, s);
    return width < max_width ? width : max_width;
}

float Painter::text_right(TextStyle style, std::wstring_view s, float right, float top, std::uint32_t rgb) {
    const float width = std::round(text_.measure(style, s));
    const float left = right - width;
    // A little extra room so the trailing letter spacing never triggers the ellipsis.
    text(style, s, left, top, width + text_style_spec(style).size_dp, rgb);
    return left;
}

}  // namespace kvm::ui
