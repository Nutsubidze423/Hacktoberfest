// SPDX-License-Identifier: Apache-2.0

#include "ui_draw.h"

#include <dwmapi.h>
#include <dwrite_3.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <stdexcept>

#include "kvm/log.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")

namespace kvm::ui {

namespace {

namespace fs = std::filesystem;

constexpr float kUnboundedWidth = 100000.0f;
constexpr int kFontSearchDepth = 5;  // exe dir + parents, e.g. build\x64-release\tools -> repo root

std::atomic<int> g_theme{-1};  // -1 = not read yet

std::string utf8(const std::wstring& s) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
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
        if (fs::exists(candidate / kFontFiles[0], ec)) {
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

bool has_family(IDWriteFactory* dwrite, IDWriteFontCollection* collection, const wchar_t* family) {
    return find_font(dwrite, collection, family, DWRITE_FONT_WEIGHT_REGULAR) != nullptr;
}

float clamp_radius(const D2D1_RECT_F& r, float radius) {
    return std::max(0.0f, std::min({radius, (r.right - r.left) / 2, (r.bottom - r.top) / 2}));
}

}  // namespace

D2D1_COLOR_F to_color(std::uint32_t rgb, float alpha) {
    return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                        static_cast<float>(rgb & 0xFF) / 255.0f, alpha);
}

// ---------------------------------------------------------------------------------------------
// Theme

Theme system_theme() {
    // SARKE_THEME=light|dark forces one (previews, screenshots, or a user who wants it fixed).
    wchar_t forced[16]{};
    if (GetEnvironmentVariableW(L"SARKE_THEME", forced, static_cast<DWORD>(std::size(forced))) > 0) {
        if (_wcsicmp(forced, L"light") == 0) return Theme::Light;
        if (_wcsicmp(forced, L"dark") == 0) return Theme::Dark;
    }
    DWORD light = 0;
    DWORD size = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS) {
        return Theme::Dark;  // the website's default too
    }
    return light ? Theme::Light : Theme::Dark;
}

Theme current_theme() {
    int t = g_theme.load();
    if (t < 0) {
        t = static_cast<int>(system_theme());
        g_theme.store(t);
    }
    return static_cast<Theme>(t);
}

bool refresh_theme() {
    const int now = static_cast<int>(system_theme());
    return g_theme.exchange(now) != now;
}

const Palette& pal() {
    return current_theme() == Theme::Light ? kLight : kDark;
}

bool is_theme_change(UINT msg, LPARAM lp) {
    return msg == WM_SETTINGCHANGE && lp != 0 && std::wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0;
}

void style_window(HWND hwnd, bool round_corners) {
    const DWM_WINDOW_CORNER_PREFERENCE corners = round_corners ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    const COLORREF no_border = DWMWA_COLOR_NONE;  // "no borders" (DESIGN.md 2): only the shadow stays
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &no_border, sizeof(no_border));
    const BOOL dark = current_theme() == Theme::Dark;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
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
    // System fallbacks for whatever the bundle didn't provide.
    if (!sans_bundled_ && !has_family(dwrite_.Get(), nullptr, sans_)) sans_ = kSansFallback2;
    if (!mono_bundled_ && !has_family(dwrite_.Get(), nullptr, mono_)) mono_ = kMonoFallback2;
}

IDWriteFontCollection* Graphics::fonts(FontRole role) const {
    const bool bundled = role == FontRole::Mono ? mono_bundled_ : sans_bundled_;
    return bundled ? bundled_.Get() : nullptr;
}

const wchar_t* Graphics::family(FontRole role) const {
    return role == FontRole::Mono ? mono_ : sans_;
}

void Graphics::load_fonts() {
    const auto dir = find_font_dir();
    if (!dir) {
        log::warn("ui: assets\\fonts\\{} not found, using {} / {}", utf8(kFontFiles[0]), utf8(kSansFallback),
                  utf8(kMonoFallback));
        return;
    }
    ComPtr<IDWriteFactory5> factory5;
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(dwrite_.As(&factory5)) || FAILED(factory5->CreateFontSetBuilder(&builder))) {
        log::warn("ui: DirectWrite font sets unavailable, using system fonts");
        return;
    }
    for (const wchar_t* file : kFontFiles) {
        ComPtr<IDWriteFontFile> font_file;
        if (FAILED(factory5->CreateFontFileReference((*dir / file).c_str(), nullptr, &font_file)) ||
            FAILED(builder->AddFontFile(font_file.Get()))) {
            log::warn("ui: cannot load {}", utf8((*dir / file).wstring()));  // that weight falls back
        }
    }
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> collection;
    if (FAILED(builder->CreateFontSet(&set)) || FAILED(factory5->CreateFontCollectionFromFontSet(set.Get(), &collection))) {
        log::warn("ui: font collection from {} failed, using system fonts", utf8(dir->wstring()));
        return;
    }
    bundled_ = collection;
    sans_bundled_ = has_family(dwrite_.Get(), bundled_.Get(), kSansFamily);
    mono_bundled_ = has_family(dwrite_.Get(), bundled_.Get(), kMonoFamily);
    if (sans_bundled_) sans_ = kSansFamily;
    if (mono_bundled_) mono_ = kMonoFamily;
    log::info("ui: fonts from {}: {}{}", utf8(dir->wstring()), sans_bundled_ ? "Google Sans" : "(no Google Sans)",
              mono_bundled_ ? ", Geist Mono" : ", (no Geist Mono)");
}

// ---------------------------------------------------------------------------------------------
// TextStyles

void TextStyles::create(UINT dpi) {
    Graphics& g = Graphics::get();
    for (int i = 0; i < kTextStyleCount; ++i) {
        const auto style = static_cast<TextStyle>(i);
        const TextStyleSpec spec = text_style_spec(style);
        const auto size = static_cast<float>(px(spec.size_dp, dpi));
        const auto line = static_cast<float>(px(spec.line_height_dp, dpi));
        const auto weight = static_cast<DWRITE_FONT_WEIGHT>(spec.weight);
        IDWriteFontCollection* collection = g.fonts(spec.font);
        const wchar_t* family = g.family(spec.font);

        Entry& e = entries_[index(style)];
        e = Entry{};
        if (FAILED(g.dwrite()->CreateTextFormat(family, collection, weight, DWRITE_FONT_STYLE_NORMAL,
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
        if (const auto font = find_font(g.dwrite(), collection, family, weight)) {
            DWRITE_FONT_METRICS fm{};
            font->GetMetrics(&fm);
            const float k = size / static_cast<float>(fm.designUnitsPerEm);
            baseline = std::round((line - (fm.ascent + fm.descent) * k) / 2.0f + fm.ascent * k);
            cap_height = fm.capHeight * k;
        }
        e.format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line, baseline);

        e.spacing_px = spec.letter_spacing_em * size;
        e.metrics = {.line_px = line, .cap_top_px = std::round(baseline - cap_height)};
    }
}

ComPtr<IDWriteTextLayout> TextStyles::layout(TextStyle style, std::wstring_view text, float max_width) const {
    const Entry& e = entries_[index(style)];
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(Graphics::get().dwrite()->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), e.format.Get(),
                                                         max_width, e.metrics.line_px, &layout))) {
        return nullptr;
    }
    if (e.spacing_px != 0 && !text.empty()) {
        ComPtr<IDWriteTextLayout1> layout1;
        if (SUCCEEDED(layout.As(&layout1))) {
            layout1->SetCharacterSpacing(0, e.spacing_px, 0, {0, static_cast<UINT32>(text.size())});
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

void Painter::round(const D2D1_RECT_F& r, float radius, std::uint32_t rgb, float alpha) {
    const float k = clamp_radius(r, radius);
    brush_->SetColor(to_color(rgb, alpha));
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, k, k), brush_);
}

void Painter::round_stroke(const D2D1_RECT_F& r, float radius, float width, std::uint32_t rgb, float alpha) {
    const float h = width / 2;  // D2D centres strokes on the outline: move it inside
    const D2D1_RECT_F in{r.left + h, r.top + h, r.right - h, r.bottom - h};
    const float k = std::max(0.0f, clamp_radius(r, radius) - h);
    brush_->SetColor(to_color(rgb, alpha));
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(in, k, k), brush_, width);
}

void Painter::pill(const D2D1_RECT_F& r, std::uint32_t rgb, float alpha) {
    round(r, (r.bottom - r.top) / 2, rgb, alpha);
}

void Painter::circle(D2D1_POINT_2F center, float radius, std::uint32_t rgb, float alpha) {
    brush_->SetColor(to_color(rgb, alpha));
    rt_->FillEllipse(D2D1::Ellipse(center, radius, radius), brush_);
}

void Painter::diagonal(D2D1_POINT_2F a, D2D1_POINT_2F b, float width, std::uint32_t rgb, float alpha) {
    brush_->SetColor(to_color(rgb, alpha));
    rt_->DrawLine(a, b, brush_, width);
}

void Painter::halo(const D2D1_RECT_F& r) {
    const Palette& c = pal();
    const float w = r.right - r.left;
    const float h = r.bottom - r.top;
    const auto one = [&](std::uint32_t rgb, float a, D2D1_POINT_2F center, float rx, float ry) {
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, to_color(rgb, a)}, {0.7f, to_color(rgb, 0.0f)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1RadialGradientBrush> brush;
        if (FAILED(rt_->CreateGradientStopCollection(stops, 2, &collection)) ||
            FAILED(rt_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(center, {0, 0}, rx, ry),
                                                  collection.Get(), &brush))) {
            return;
        }
        rt_->FillRectangle(r, brush.Get());
    };
    // globals.css body::before: 1100x560 at 50% -8%, and 900x640 at 105% 105% (of a 1440x900 page).
    one(c.halo1, alpha::halo1, {r.left + w * 0.5f, r.top - h * 0.08f}, w * 0.76f, h * 0.62f);
    one(c.halo2, alpha::halo2, {r.left + w * 1.05f, r.top + h * 1.05f}, w * 0.62f, h * 0.71f);
}

void Painter::logo(const D2D1_RECT_F& r) {
    const float s = r.right - r.left;
    const float sq = std::round(s * logo::square);
    const float inset = std::round(s * logo::inset);
    const float k = std::max(1.0f, s * logo::square_radius);
    const float stroke = std::max(1.0f, std::round(s * logo::stroke));
    round(r, s * logo::tile_radius, logo::tile);
    round({r.left + inset, r.top + inset, r.left + inset + sq, r.top + inset + sq}, k, logo::mark);
    const D2D1_RECT_F back{r.right - inset - sq, r.bottom - inset - sq, r.right - inset, r.bottom - inset};
    round(back, k, logo::tile);
    round_stroke(back, k, stroke, logo::mark);
}

// ---------------------------------------------------------------------------------------------
// LayeredSurface

LayeredSurface::~LayeredSurface() {
    release();
}

void LayeredSurface::release() {
    rt_.Reset();
    brush_.Reset();
    if (dc_) {
        if (old_) SelectObject(dc_, old_);
        DeleteDC(dc_);
    }
    if (bitmap_) DeleteObject(bitmap_);
    dc_ = nullptr;
    bitmap_ = nullptr;
    old_ = nullptr;
    bits_ = nullptr;
    w_ = h_ = 0;
}

ID2D1RenderTarget* LayeredSurface::begin(int w, int h) {
    if (w <= 0 || h <= 0) return nullptr;
    if (w != w_ || h != h_ || !dc_) {
        release();
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        dc_ = CreateCompatibleDC(nullptr);
        bitmap_ = dc_ ? CreateDIBSection(dc_, &bmi, DIB_RGB_COLORS, &bits_, nullptr, 0) : nullptr;
        if (!bitmap_ || !bits_) {
            release();
            return nullptr;
        }
        old_ = SelectObject(dc_, bitmap_);
        w_ = w;
        h_ = h;
    }
    if (!rt_) {
        const auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        if (FAILED(Graphics::get().d2d()->CreateDCRenderTarget(&props, &rt_))) return nullptr;
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt_->CreateSolidColorBrush(to_color(0), &brush_);
    }
    const RECT rc{0, 0, w, h};
    if (FAILED(rt_->BindDC(dc_, &rc))) return nullptr;
    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    return rt_.Get();
}

bool LayeredSurface::end() {
    if (!rt_) return false;
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) {
        rt_.Reset();
        brush_.Reset();
        return false;
    }
    GdiFlush();
    return true;
}

void LayeredSurface::present(HWND hwnd, int x, int y, BYTE alpha) const {
    if (!dc_) return;
    POINT dst{x, y};
    SIZE size{w_, h_};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, nullptr, &dst, &size, dc_, &src, 0, &blend, ULW_ALPHA);
}

// ---------------------------------------------------------------------------------------------
// Painter text

float Painter::text(TextStyle style, std::wstring_view s, float x, float top, float max_width, std::uint32_t rgb,
                    float alpha) {
    if (s.empty() || max_width <= 0) {
        return 0;
    }
    const auto layout = text_.layout(style, s, max_width);
    if (!layout) {
        return 0;
    }
    brush_->SetColor(to_color(rgb, alpha));
    rt_->DrawTextLayout({std::round(x), std::round(top)}, layout.Get(), brush_);
    const float width = text_.measure(style, s);
    return width < max_width ? width : max_width;
}

float Painter::text_right(TextStyle style, std::wstring_view s, float right, float top, std::uint32_t rgb) {
    const float width = std::round(text_.measure(style, s));
    const float left = right - width;
    // A little extra room so trailing letter spacing never triggers the ellipsis.
    text(style, s, left, top, width + text_style_spec(style).size_dp, rgb);
    return left;
}

float Painter::text_center(TextStyle style, std::wstring_view s, const D2D1_RECT_F& r, std::uint32_t rgb, float alpha) {
    const float width = std::round(text_.measure(style, s));
    const float line = text_.metrics(style).line_px;
    const float left = std::round((r.left + r.right - width) / 2);
    text(style, s, left, std::round((r.top + r.bottom - line) / 2), width + text_style_spec(style).size_dp, rgb, alpha);
    return left;
}

}  // namespace kvm::ui
