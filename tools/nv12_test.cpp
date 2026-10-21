// SPDX-License-Identifier: Apache-2.0

// nv12_test: BGRA -> NV12 on the GPU (host_a/nv12_converter.h, roadmap P2.3a). Needs a GPU with a D3D11
// video processor (ctest label "gpu": local only, CI runners have none). Checks:
//   - red, green, blue, white and gray blocks come out as BT.709 limited-range Y / Cb / Cr;
//   - an odd size is rounded down to even;
//   - the time for a 1920x1080 conversion (printed).
//
//   nv12_test.exe   (exit code 0 = all passed)

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string_view>
#include <vector>

#include "kvm/gpu_device.h"
#include "kvm/log.h"
#include "nv12_converter.h"

using Microsoft::WRL::ComPtr;

namespace {

struct Checks {
    int failed = 0;
    void operator()(bool ok, std::string_view what) {
        kvm::log::info("{} {}", ok ? "[ok]  " : "[FAIL]", what);
        if (!ok) ++failed;
    }
};

struct Rgb {
    const char* name;
    std::uint8_t r, g, b;
};

// BT.709, limited range: what the converter is asked for.
struct Yuv {
    double y, cb, cr;
};
Yuv bt709_limited(const Rgb& c) {
    const double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
    const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    return {16 + 219 * y, 128 + 224 * (b - y) / 1.8556, 128 + 224 * (r - y) / 1.5748};
}

ComPtr<ID3D11Texture2D> bgra_texture(ID3D11Device* device, int width, int height,
                                     const std::vector<std::uint8_t>& pixels) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(width);
    d.Height = static_cast<UINT>(height);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;  // like WGC's: no render target
    const D3D11_SUBRESOURCE_DATA data{pixels.data(), static_cast<UINT>(width * 4), 0};
    ComPtr<ID3D11Texture2D> t;
    device->CreateTexture2D(&d, pixels.empty() ? nullptr : &data, &t);
    return t;
}

// The NV12 texture's bytes: Y rows, then interleaved Cb Cr rows of half height.
struct Nv12 {
    int width = 0, height = 0;
    std::vector<std::uint8_t> y, uv;
};
Nv12 read_back(kvm::GpuDevice& gpu, ID3D11Texture2D* nv12) {
    D3D11_TEXTURE2D_DESC d{};
    nv12->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Nv12 out{static_cast<int>(d.Width), static_cast<int>(d.Height)};
    if (FAILED(gpu.device()->CreateTexture2D(&d, nullptr, &staging))) return out;
    const kvm::GpuDevice::Lock lock(gpu);
    gpu.context()->CopyResource(staging.Get(), nv12);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(gpu.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return out;
    const auto* base = static_cast<const std::uint8_t*>(m.pData);
    for (UINT row = 0; row < d.Height; ++row)
        out.y.insert(out.y.end(), base + row * m.RowPitch, base + row * m.RowPitch + d.Width);
    const auto* uv = base + static_cast<std::size_t>(m.RowPitch) * d.Height;
    for (UINT row = 0; row < d.Height / 2; ++row)
        out.uv.insert(out.uv.end(), uv + row * m.RowPitch, uv + row * m.RowPitch + d.Width);
    gpu.context()->Unmap(staging.Get(), 0);
    return out;
}

}  // namespace

int main() {
    Checks check;
    auto gpu = kvm::GpuDevice::create();
    check(gpu != nullptr, "a hardware D3D11 device with video support");
    if (!gpu) return 1;
    kvm::log::info("       adapter: {}", gpu->adapter_name());
    kvm::Nv12Converter nv12(gpu);

    kvm::log::info("colours");
    {
        const Rgb colours[] = {{"red", 255, 0, 0},
                               {"green", 0, 255, 0},
                               {"blue", 0, 0, 255},
                               {"white", 255, 255, 255},
                               {"gray", 128, 128, 128}};
        constexpr int kBlock = 32, kHeight = 32;
        const int width = kBlock * static_cast<int>(std::size(colours));
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * kHeight * 4);
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < width; ++x) {
                const Rgb& c = colours[x / kBlock];
                std::uint8_t* p = &pixels[(static_cast<std::size_t>(y) * width + x) * 4];
                p[0] = c.b, p[1] = c.g, p[2] = c.r, p[3] = 255;
            }
        }
        auto src = bgra_texture(gpu->device(), width, kHeight, pixels);
        ID3D11Texture2D* out = nv12.convert(src.Get(), width, kHeight);
        check(out != nullptr, "the converter takes a WGC-like texture (no render-target binding)");
        if (out) {
            const Nv12 n = read_back(*gpu, out);
            for (std::size_t i = 0; i < std::size(colours); ++i) {
                // The middle of each block: no blending with its neighbours.
                const int x = static_cast<int>(i) * kBlock + kBlock / 2, y = kHeight / 2;
                const double got_y = n.y[static_cast<std::size_t>(y) * n.width + x];
                const std::size_t uv_at = static_cast<std::size_t>(y / 2) * n.width + (x / 2) * 2;
                const double got_cb = n.uv[uv_at], got_cr = n.uv[uv_at + 1];
                const Yuv want = bt709_limited(colours[i]);
                const bool ok =
                    std::abs(got_y - want.y) <= 3 && std::abs(got_cb - want.cb) <= 3 && std::abs(got_cr - want.cr) <= 3;
                check(ok, std::format("{}: Y/Cb/Cr {:.0f}/{:.0f}/{:.0f}, BT.709 limited {:.0f}/{:.0f}/{:.0f}",
                                      colours[i].name, got_y, got_cb, got_cr, want.y, want.cb, want.cr));
            }
        }
    }

    kvm::log::info("sizes");
    {
        auto src = bgra_texture(gpu->device(), 101, 51, std::vector<std::uint8_t>(101 * 51 * 4, 200));
        ID3D11Texture2D* out = nv12.convert(src.Get(), 101, 51);
        D3D11_TEXTURE2D_DESC d{};
        if (out) out->GetDesc(&d);
        check(out && d.Width == 100 && d.Height == 50 && nv12.width() == 100 && nv12.height() == 50,
              "101x51 becomes 100x50 (NV12 needs even sizes)");
    }

    kvm::log::info("speed");
    {
        auto src = bgra_texture(gpu->device(), 1920, 1080, std::vector<std::uint8_t>(1920 * 1080 * 4, 90));
        constexpr int kRuns = 60;
        nv12.convert(src.Get(), 1920, 1080);  // set-up for this size happens once
        const auto t0 = std::chrono::steady_clock::now();
        ID3D11Texture2D* out = nullptr;
        for (int i = 0; i < kRuns; ++i) out = nv12.convert(src.Get(), 1920, 1080);
        const Nv12 n = out ? read_back(*gpu, out) : Nv12{};  // waits for the GPU to finish all of them
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kRuns;
        check(n.width == 1920 && n.height == 1080, std::format("1920x1080: {:.2f} ms per conversion (GPU)", ms));
    }

    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
