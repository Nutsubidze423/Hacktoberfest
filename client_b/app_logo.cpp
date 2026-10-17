#include "app_logo.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include "ui_tokens_tmp.h"

namespace kvm::ui {

namespace {

// 0xRRGGBB -> opaque BGRA as stored in a 32-bpp DIB.
std::uint32_t to_bgra(std::uint32_t rgb) {
    return 0xFF000000u | rgb;  // a DIB pixel 0xAARRGGBB is laid out as B, G, R, A in memory
}

}  // namespace

unique_icon create_logo_icon(int size) {
    if (size <= 0) {
        return nullptr;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HBITMAP color = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) {
        return nullptr;
    }

    auto* pixels = static_cast<std::uint32_t*>(bits);
    const std::uint32_t background = to_bgra(logo::background);
    for (int i = 0; i < size * size; ++i) {
        pixels[i] = background;
    }
    // Grid line g sits at pixel round(g * size / grid): crisp edges at any size (16, 20, 24, 32...).
    const auto edge = [size](int g) {
        return static_cast<int>(std::lround(static_cast<double>(g) * size / logo::grid));
    };
    for (const logo::Cell& cell : logo::cells) {
        const std::uint32_t c = to_bgra(cell.rgb);
        for (int y = edge(cell.y); y < edge(cell.y + cell.h); ++y) {
            for (int x = edge(cell.x); x < edge(cell.x + cell.w); ++x) {
                pixels[y * size + x] = c;
            }
        }
    }

    // All-zero AND mask: the 32-bpp color bitmap's alpha (fully opaque) decides.
    const std::vector<BYTE> mask_bits(static_cast<std::size_t>((size + 15) / 16 * 2 * size), 0);
    const HBITMAP mask = CreateBitmap(size, size, 1, 1, mask_bits.data());

    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmMask = mask;
    info.hbmColor = color;
    unique_icon icon(mask ? CreateIconIndirect(&info) : nullptr);
    DeleteObject(color);
    if (mask) {
        DeleteObject(mask);
    }
    return icon;
}

}  // namespace kvm::ui
