// SPDX-License-Identifier: Apache-2.0

// jpeg_test: TurboJPEG speed and output check (T5). Needs no VDD and no network.
//
//   jpeg_test.exe [frame.bmp] [--runs 100] [--quality 75]
//
// Encodes a capture_test BMP (or, without one, a synthetic 1920x1080 screen) `runs` times, prints
// the time per frame, decodes the result to check it, and saves captures/jpeg_test.jpg.
// Quality defaults to config.json jpeg_quality. Target: < 15 ms per 1920x1080 frame.

#include <turbojpeg.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "jpeg_encoder.h"
#include "kvm/config.h"
#include "kvm/log.h"

namespace {

// Reads a 32-bit uncompressed BMP (what capture_test writes), either row order.
std::optional<kvm::CapturedFrame> load_bmp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    if (!in.read(reinterpret_cast<char*>(&file), sizeof(file)) || !in.read(reinterpret_cast<char*>(&info), sizeof(info)) ||
        file.bfType != 0x4D42 || info.biBitCount != 32 || info.biCompression != BI_RGB) {
        return std::nullopt;
    }
    kvm::CapturedFrame f;
    f.width = info.biWidth;
    f.height = std::abs(info.biHeight);
    f.stride = f.width * 4;
    f.bgra.resize(static_cast<size_t>(f.stride) * f.height);
    in.seekg(file.bfOffBits);
    for (int row = 0; row < f.height; ++row) {
        const int y = info.biHeight < 0 ? row : f.height - 1 - row;  // bottom-up BMPs start with the last row
        in.read(reinterpret_cast<char*>(f.bgra.data() + static_cast<size_t>(y) * f.stride), f.stride);
    }
    if (!in) return std::nullopt;
    return f;
}

// Something screen-like: flat backgrounds, window frames and lines of "text" (sharp, busy detail).
kvm::CapturedFrame synthetic_screen(int width, int height) {
    kvm::CapturedFrame f;
    f.width = width;
    f.height = height;
    f.stride = width * 4;
    f.bgra.resize(static_cast<size_t>(f.stride) * height);
    std::uint32_t seed = 12345;
    const auto rnd = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 16;
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto* p = f.bgra.data() + static_cast<size_t>(y) * f.stride + static_cast<size_t>(x) * 4;
            std::uint8_t b = 243, g = 243, r = 243;
            if (y < 40) b = g = r = 32;                                    // title bar
            else if (x < 260) { b = 230; g = 220; r = 210; }               // side panel
            else if (y % 28 < 16 && x < width - 300 && rnd() % 3 == 0) {   // text lines
                b = g = r = static_cast<std::uint8_t>(20 + rnd() % 60);
            }
            if (x > 900 && x < 1500 && y > 300 && y < 700) {               // an image area
                b = static_cast<std::uint8_t>(x / 7);
                g = static_cast<std::uint8_t>(y / 3);
                r = static_cast<std::uint8_t>((x + y) / 9);
            }
            p[0] = b;
            p[1] = g;
            p[2] = r;
            p[3] = 255;
        }
    }
    return f;
}

std::optional<std::pair<int, int>> jpeg_size(const std::vector<std::uint8_t>& jpeg) {
    tjhandle h = tj3Init(TJINIT_DECOMPRESS);
    std::optional<std::pair<int, int>> out;
    if (h && tj3DecompressHeader(h, jpeg.data(), jpeg.size()) == 0) {
        out = std::pair{tj3Get(h, TJPARAM_JPEGWIDTH), tj3Get(h, TJPARAM_JPEGHEIGHT)};
    }
    tj3Destroy(h);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::optional<std::filesystem::path> bmp;
    int runs = 100;
    std::optional<int> quality;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--runs" && i + 1 < argc) runs = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--quality" && i + 1 < argc) quality = std::atoi(argv[++i]);
        else bmp = arg;
    }
    kvm::Config config;
    try {
        config = kvm::load_config();
    } catch (const std::exception& e) {
        kvm::log::warn("config.json: {}; using defaults", e.what());
    }
    if (!quality) quality = config.jpeg_quality;
    if (!bmp && std::filesystem::exists("captures/frame_09.bmp")) bmp = "captures/frame_09.bmp";

    std::optional<kvm::CapturedFrame> frame;
    if (bmp) {
        frame = load_bmp(*bmp);
        if (!frame) kvm::log::warn("can't read {} as a 32-bit BMP, using a synthetic screen", bmp->string());
    }
    const bool real = frame.has_value();
    if (!frame) frame = synthetic_screen(1920, 1080);
    kvm::log::info("input: {} {}x{}, quality {}", real ? bmp->string() : std::string("synthetic screen"),
                   frame->width, frame->height, *quality);

    kvm::JpegEncoder encoder(*quality);
    std::vector<std::uint8_t> jpeg;
    std::vector<double> ms;
    ms.reserve(static_cast<size_t>(runs));
    for (int i = 0; i < runs; ++i) {
        if (!encoder.encode(*frame, jpeg)) return 1;
        ms.push_back(encoder.last_encode_ms());
    }
    std::sort(ms.begin(), ms.end());
    const double avg = std::accumulate(ms.begin(), ms.end(), 0.0) / static_cast<double>(ms.size());
    const double p95 = ms[std::min(ms.size() - 1, ms.size() * 95 / 100)];
    kvm::log::info("encode: avg {:.2f} ms, min {:.2f} ms, p95 {:.2f} ms over {} runs", avg, ms.front(), p95, runs);
    kvm::log::info("size: {:.0f} KB per frame = {:.1f} Mbit/s at {} fps", static_cast<double>(jpeg.size()) / 1024.0,
                   static_cast<double>(jpeg.size()) * 8.0 * config.max_fps / 1e6, config.max_fps);

    std::filesystem::create_directories("captures");
    std::ofstream("captures/jpeg_test.jpg", std::ios::binary)
        .write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));

    const auto decoded = jpeg_size(jpeg);
    const bool valid = decoded && decoded->first == frame->width && decoded->second == frame->height;
    kvm::log::info("{} decodes back to {}x{} (captures/jpeg_test.jpg)", valid ? "[ok]  " : "[FAIL]",
                   decoded ? decoded->first : 0, decoded ? decoded->second : 0);
    kvm::log::info("{} avg {:.2f} ms vs target 15 ms", avg < 15.0 ? "[ok]  " : "[slow]", avg);
    return valid ? 0 : 1;
}
