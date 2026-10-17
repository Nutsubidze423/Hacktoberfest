// ui_preview_logo: the app logo (docs/DESIGN.md section 9) at every icon size Windows uses, saved
// as one image to check the pixels (docs/DESIGN.md section 7). Rows, top to bottom:
//   1. exact sizes 16 20 24 28 32 40 48 64 96 128 px on a dark taskbar
//   2. the same on a light taskbar
//   3. 16 / 20 / 24 / 32 px magnified 8x (nearest neighbor): 100 / 125 / 150 / 200 % tray icons
//
//   ui_preview_logo.exe [out.bmp]        default: sarke_logo_sizes.bmp next to the exe

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

#include "app_logo.h"
#include "kvm/log.h"

namespace fs = std::filesystem;

namespace {

constexpr int kSizes[] = {16, 20, 24, 28, 32, 40, 48, 64, 96, 128};
constexpr int kMagnified[] = {16, 20, 24, 32};
constexpr int kZoom = 8;
constexpr int kGap = 24;
constexpr COLORREF kDarkTaskbar = RGB(0x20, 0x20, 0x20);
constexpr COLORREF kLightTaskbar = RGB(0xF3, 0xF3, 0xF3);
constexpr COLORREF kZoomBackground = RGB(0x3A, 0x3A, 0x40);  // lineStrong: shows the tile's own edge

struct DcDeleter {
    void operator()(HDC dc) const { DeleteDC(dc); }
};
struct GdiDeleter {
    void operator()(HGDIOBJ obj) const { DeleteObject(obj); }
};

std::string utf8(const std::wstring& s) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

void fill(HDC dc, int x, int y, int w, int h, COLORREF color) {
    const RECT r{x, y, x + w, y + h};
    std::unique_ptr<std::remove_pointer_t<HBRUSH>, GdiDeleter> brush(CreateSolidBrush(color));
    FillRect(dc, &r, brush.get());
}

// 32-bpp BMP, bottom-up rows (the most widely supported layout).
bool save_bmp(const fs::path& path, const std::uint32_t* top_down, int width, int height) {
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = height;  // positive = bottom-up
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    const auto image_bytes = static_cast<DWORD>(width) * static_cast<DWORD>(height) * 4;
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;  // "BM"
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + image_bytes;

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    for (int y = height - 1; y >= 0; --y) {
        out.write(reinterpret_cast<const char*>(top_down + static_cast<std::size_t>(y) * width),
                  static_cast<std::streamsize>(width) * 4);
    }
    return static_cast<bool>(out);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    fs::path out_path;
    if (argc >= 2) {
        out_path = argv[1];
    } else {
        std::wstring exe(MAX_PATH, L'\0');
        exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
        out_path = fs::path(exe).parent_path() / L"sarke_logo_sizes.bmp";
    }

    const int largest = *std::max_element(std::begin(kSizes), std::end(kSizes));
    const int row_h = largest + 2 * kGap;
    const int zoom_row_h = kMagnified[std::size(kMagnified) - 1] * kZoom + 2 * kGap;
    const int sizes_w = std::accumulate(std::begin(kSizes), std::end(kSizes), kGap,
                                        [](int acc, int s) { return acc + s + kGap; });
    const int zoom_w = std::accumulate(std::begin(kMagnified), std::end(kMagnified), kGap,
                                       [](int acc, int s) { return acc + s * kZoom + kGap; });
    const int width = std::max(sizes_w, zoom_w);
    const int height = 2 * row_h + zoom_row_h;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // top-down in memory
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    std::unique_ptr<std::remove_pointer_t<HDC>, DcDeleter> dc(CreateCompatibleDC(nullptr));
    std::unique_ptr<std::remove_pointer_t<HBITMAP>, GdiDeleter> dib(
        CreateDIBSection(dc.get(), &bmi, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!dc || !dib || !bits) {
        kvm::log::error("could not create the drawing surface");
        return 1;
    }
    const HGDIOBJ old = SelectObject(dc.get(), dib.get());

    fill(dc.get(), 0, 0, width, row_h, kDarkTaskbar);
    fill(dc.get(), 0, row_h, width, row_h, kLightTaskbar);
    fill(dc.get(), 0, 2 * row_h, width, zoom_row_h, kZoomBackground);

    // Rows 1 and 2: every size at its exact pixel size, bottoms aligned.
    std::vector<POINT> dark_positions;
    int x = kGap;
    for (const int size : kSizes) {
        const auto icon = kvm::ui::create_logo_icon(size);
        const int y = kGap + largest - size;
        DrawIconEx(dc.get(), x, y, icon.get(), size, size, 0, nullptr, DI_NORMAL);
        DrawIconEx(dc.get(), x, row_h + y, icon.get(), size, size, 0, nullptr, DI_NORMAL);
        dark_positions.push_back({x, y});
        x += size + kGap;
    }

    // Row 3: the small sizes magnified, copied pixel for pixel from row 1.
    SetStretchBltMode(dc.get(), COLORONCOLOR);
    x = kGap;
    for (const int size : kMagnified) {
        const auto it = std::find(std::begin(kSizes), std::end(kSizes), size);
        const POINT src = dark_positions[static_cast<std::size_t>(it - std::begin(kSizes))];
        const int y = 2 * row_h + kGap;
        StretchBlt(dc.get(), x, y, size * kZoom, size * kZoom, dc.get(), src.x, src.y, size, size, SRCCOPY);
        x += size * kZoom + kGap;
    }
    GdiFlush();

    const bool saved = save_bmp(out_path, static_cast<const std::uint32_t*>(bits), width, height);
    SelectObject(dc.get(), old);
    if (!saved) {
        kvm::log::error("could not write {}", utf8(out_path.wstring()));
        return 1;
    }
    kvm::log::info("saved {} ({}x{}): sizes 16 20 24 28 32 40 48 64 96 128 px; magnified 16 20 24 32",
                   utf8(out_path.wstring()), width, height);
    return 0;
}
