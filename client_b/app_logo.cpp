#include "app_logo.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <vector>

#include "ui_draw_tmp.h"

namespace kvm::ui {

unique_icon create_logo_icon(int size) {
    if (size <= 0) {
        return nullptr;
    }
    // The mark is drawn with Direct2D (antialiased rounded tile, transparent corners).
    LayeredSurface canvas;
    try {
        ID2D1RenderTarget* rt = canvas.begin(size, size);
        if (!rt) {
            return nullptr;
        }
        TextStyles no_text;  // the logo has no text
        Painter p(rt, canvas.brush(), no_text);
        const auto s = static_cast<float>(size);
        p.logo({0, 0, s, s});
        if (!canvas.end()) {
            return nullptr;
        }
    } catch (const std::exception&) {
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
        if (color) DeleteObject(color);
        return nullptr;
    }
    // Icons take straight (not premultiplied) alpha.
    auto* out = static_cast<std::uint32_t*>(bits);
    const std::uint32_t* in = canvas.pixels();
    for (int i = 0; i < size * size; ++i) {
        const std::uint32_t px = in[i];
        const std::uint32_t a = px >> 24;
        if (a == 0) {
            out[i] = 0;
            continue;
        }
        const auto un = [&](int shift) { return (((px >> shift) & 0xFF) * 255 + a / 2) / a; };
        out[i] = (a << 24) | (std::min<std::uint32_t>(255, un(16)) << 16) | (std::min<std::uint32_t>(255, un(8)) << 8) |
                 std::min<std::uint32_t>(255, un(0));
    }

    // All-zero AND mask: the 32-bpp color bitmap's alpha decides.
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
