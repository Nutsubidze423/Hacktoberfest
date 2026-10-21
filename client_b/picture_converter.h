// SPDX-License-Identifier: Apache-2.0

// Decoded H.264 pictures (h264_decoder.h) to BGRA for the viewer (roadmap P2.4b), BT.709 limited
// range in, full-range RGB out:
//   - on the GPU, with the D3D11 video processor: the decoder's NV12 texture is copied (so the
//     decoder has it back at once) and converted into a BGRA texture the viewer draws;
//   - on the CPU, for a picture decoded in software, into a BgraImage.
//
//   kvm::PictureConverter convert(viewer.gpu());
//   if (auto bgra = convert.to_texture(*picture)) viewer.present_texture(bgra, picture->width, picture->height, info);
//   kvm::PictureConverter::to_bgra(*picture, image);      // picture->nv12 (software)
//
// The BGRA textures are a ring of three: the viewer shows one, the next waits, the third is written.
// One thread converts.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <memory>

#include "h264_decoder.h"
#include "jpeg_decoder.h"
#include "kvm/gpu_device.h"

namespace kvm {

class PictureConverter {
public:
    explicit PictureConverter(std::shared_ptr<GpuDevice> gpu);

    // The picture as a BGRA texture of the device (shader resource), `picture.width` x `.height`.
    // nullptr if it is not a texture of this device or the video processor can't convert it.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> to_texture(const H264Decoder::Picture& picture);

    // A picture decoded in memory (picture.nv12), on the CPU. False if it has no pixels.
    static bool to_bgra(const H264Decoder::Picture& picture, BgraImage& out);

private:
    bool prepare(const D3D11_TEXTURE2D_DESC& decoded, int width, int height);

    std::shared_ptr<GpuDevice> gpu_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> input_;  // NV12, the decoder's texture size
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 3> outputs_;
    std::array<Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView>, 3> output_views_;
    std::size_t next_ = 0;
    UINT input_w_ = 0;
    UINT input_h_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool warned_ = false;
};

}  // namespace kvm
