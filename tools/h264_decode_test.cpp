// SPDX-License-Identifier: Apache-2.0

// h264_decode_test: client_b/h264_decoder.h in software, on a stream Sarke's encoder made
// (tools/data/h264_sample.h264, written by `h264_test --write-sample`: 10 pictures of 320x240, red,
// green, blue and gray quadrants and a white block moving along the middle). No GPU needed, so it
// runs in CI. It checks:
//   - every access unit passes the viewer's framing check (kvm/annexb.h);
//   - each decode() returns that access unit's own picture (low latency: nothing held back), 320x240,
//     in memory;
//   - the quadrants decode to their BT.709 limited-range colours (within a few levels).
// The GPU path (DXVA, D3D11 textures, PSNR at 1080p) is h264_test's (label gpu).
//
//   h264_decode_test.exe <h264_sample.h264>   (exit code 0 = all passed)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "h264_decoder.h"
#include "kvm/annexb.h"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) {
        ++g_failures;
    }
}

constexpr int kWidth = 320, kHeight = 240, kPictures = 10, kTolerance = 6;

// The stream's access units, each copied out.
std::vector<std::vector<std::uint8_t>> access_units(const std::vector<std::uint8_t>& stream) {
    std::vector<std::vector<std::uint8_t>> out;
    for (const auto& [first, second] : kvm::annexb::access_units(stream)) {
        out.emplace_back(stream.begin() + static_cast<std::ptrdiff_t>(first),
                         stream.begin() + static_cast<std::ptrdiff_t>(second));
    }
    return out;
}

struct YCbCr {
    int y, cb, cr;
};

// BT.709, limited range, from 8-bit R'G'B' (what the encoder side's video processor does).
YCbCr bt709(double r, double g, double b) {
    const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    return {static_cast<int>(16.0 + 219.0 * y / 255.0 + 0.5),
            static_cast<int>(128.0 + 224.0 * (b - y) / (1.8556 * 255.0) + 0.5),
            static_cast<int>(128.0 + 224.0 * (r - y) / (1.5748 * 255.0) + 0.5)};
}

// The average colour of the 8x8 block at (x, y) of an NV12 picture in memory.
YCbCr sample(const kvm::H264Decoder::Picture& p, int x, int y) {
    const auto stride = static_cast<std::size_t>(p.stride);
    const std::uint8_t* uv = p.nv12.data() + stride * static_cast<std::size_t>(p.coded_height);
    int sy = 0, scb = 0, scr = 0;
    for (int dy = 0; dy < 8; ++dy) {
        for (int dx = 0; dx < 8; ++dx) {
            const auto row = static_cast<std::size_t>(y + dy), col = static_cast<std::size_t>(x + dx);
            sy += p.nv12[row * stride + col];
            scb += uv[(row / 2) * stride + (col / 2) * 2];
            scr += uv[(row / 2) * stride + (col / 2) * 2 + 1];
        }
    }
    return {sy / 64, scb / 64, scr / 64};
}

int distance(const YCbCr& got, const YCbCr& want) {
    return std::max({std::abs(got.y - want.y), std::abs(got.cb - want.cb), std::abs(got.cr - want.cr)});
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("usage: h264_decode_test.exe <h264_sample.h264>\n");
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> stream((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto units = access_units(stream);
    check(units.size() == kPictures, std::to_string(units.size()) + " access units in the sample");

    std::size_t framed = 0;
    for (std::size_t i = 0; i < units.size(); ++i) {
        if (kvm::annexb::check(units[i], i == 0).empty()) ++framed;
    }
    check(framed == units.size(), std::to_string(framed) + " pass the framing check (the first is the keyframe)");

    std::string why;
    auto decoder = kvm::H264Decoder::create(nullptr, why);
    check(decoder != nullptr, decoder ? "a software decoder: " + decoder->name() : why);
    if (!decoder) return 1;

    // Quadrant centres and the colour each should be.
    struct Spot {
        int x, y;
        YCbCr want;
        const char* name;
    };
    const Spot spots[] = {{76, 56, bt709(255, 0, 0), "red"},
                          {236, 56, bt709(0, 255, 0), "green"},
                          {76, 176, bt709(0, 0, 255), "blue"},
                          {236, 176, bt709(128, 128, 128), "gray"}};
    std::size_t at_once = 0, right_size = 0, right_colour = 0;
    int worst = 0;
    for (std::size_t i = 0; i < units.size(); ++i) {
        const auto picture = decoder->decode(units[i], static_cast<std::int64_t>(i) * 33'333);
        if (!picture) {
            std::printf("       access unit %zu: no picture yet\n", i);
            continue;
        }
        if (picture->t_capture_us == static_cast<std::int64_t>(i) * 33'333) {
            ++at_once;
        } else {
            std::printf("       access unit %zu gave picture %lld\n", i, picture->t_capture_us / 33'333);
        }
        if (picture->width != kWidth || picture->height != kHeight || picture->nv12.empty()) continue;
        ++right_size;
        bool colours = true;
        for (const auto& spot : spots) {
            const auto got = sample(*picture, spot.x, spot.y);
            worst = std::max(worst, distance(got, spot.want));
            if (distance(got, spot.want) > kTolerance) {
                std::printf("       picture %zu, %s: got %d/%d/%d, want %d/%d/%d\n", i, spot.name, got.y, got.cb,
                            got.cr, spot.want.y, spot.want.cb, spot.want.cr);
                colours = false;
            }
        }
        if (colours) ++right_colour;
    }
    check(!decoder->on_gpu(), "decoded in memory (no GPU given)");
    check(at_once == units.size() && decoder->errors() == 0,
          std::to_string(at_once) + " of " + std::to_string(units.size()) +
              " decode() calls returned their own picture (low latency)");
    check(right_size == units.size(), std::to_string(right_size) + " pictures are 320x240");
    check(right_colour == units.size(), std::to_string(right_colour) + " pictures have red, green, blue and gray " +
                                            "quadrants within " + std::to_string(kTolerance) +
                                            " levels (worst: " + std::to_string(worst) + ")");

    std::printf("\n%s\n", g_failures == 0 ? "all passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
