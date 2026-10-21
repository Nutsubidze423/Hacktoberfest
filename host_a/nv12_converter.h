// SPDX-License-Identifier: Apache-2.0

// BGRA -> NV12 on the GPU with the D3D11 video processor (roadmap P2.3a): NV12 is what the hardware
// H.264 encoder takes. The colour space is set on both sides, BT.709 RGB in, BT.709 limited range
// (16-235) YCbCr out: a mismatch shows as washed-out or crushed colours (ROADMAP Phase 2 risks).
//
//   kvm::Nv12Converter nv12(gpu);
//   ID3D11Texture2D* out = nv12.convert(bgra, width, height);  // on the GPU; valid until the next call
//
// One thread at a time; it takes the GpuDevice lock around the immediate context itself.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>

#include "gpu_device.h"

namespace kvm {

class Nv12Converter {
public:
    explicit Nv12Converter(std::shared_ptr<GpuDevice> gpu);

    // Converts the top-left width x height of `bgra` (a DXGI_FORMAT_B8G8R8A8_UNORM texture, any bind
    // flags) into an NV12 texture of that size rounded down to even numbers (NV12 has half-size
    // chroma). nullptr on failure (logged).
    ID3D11Texture2D* convert(ID3D11Texture2D* bgra, int width, int height);

    int width() const { return width_; }  // of the last NV12 picture
    int height() const { return height_; }

private:
    bool prepare(int width, int height);  // the processor, views and textures for this size

    std::shared_ptr<GpuDevice> gpu_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> input_;  // a copy of the picture with the bind flags it needs
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view_;
    int width_ = 0;
    int height_ = 0;
    bool warned_ = false;
};

}  // namespace kvm
