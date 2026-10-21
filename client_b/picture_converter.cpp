// SPDX-License-Identifier: Apache-2.0

#include "picture_converter.h"

#include <d3d11_1.h>

#include <algorithm>
#include <utility>

#include "kvm/log.h"

namespace kvm {

namespace {

using Microsoft::WRL::ComPtr;

std::uint8_t clamp_byte(int v) {
    return static_cast<std::uint8_t>(std::clamp(v, 0, 255));
}

}  // namespace

PictureConverter::PictureConverter(std::shared_ptr<GpuDevice> gpu) : gpu_(std::move(gpu)) {
    if (!gpu_ || !gpu_->has_video() || FAILED(gpu_->device()->QueryInterface(IID_PPV_ARGS(&video_device_))) ||
        FAILED(gpu_->context()->QueryInterface(IID_PPV_ARGS(&video_context_)))) {
        video_device_.Reset();
    }
}

bool PictureConverter::prepare(const D3D11_TEXTURE2D_DESC& decoded, int width, int height) {
    if (processor_ && decoded.Width == input_w_ && decoded.Height == input_h_ && width == width_ && height == height_) {
        return true;
    }
    processor_.Reset();
    enumerator_.Reset();
    input_.Reset();
    input_view_.Reset();
    outputs_ = {};
    output_views_ = {};
    width_ = height_ = 0;

    const auto w = static_cast<UINT>(width), h = static_cast<UINT>(height);
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = decoded.Width;
    content.InputHeight = decoded.Height;
    content.OutputWidth = w;
    content.OutputHeight = h;
    content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(video_device_->CreateVideoProcessorEnumerator(&content, &enumerator_)) ||
        FAILED(video_device_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_))) {
        return false;
    }

    D3D11_TEXTURE2D_DESC in{};
    in.Width = decoded.Width;
    in.Height = decoded.Height;
    in.MipLevels = 1;
    in.ArraySize = 1;
    in.Format = DXGI_FORMAT_NV12;
    in.SampleDesc.Count = 1;
    in.Usage = D3D11_USAGE_DEFAULT;
    in.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_view{};
    in_view.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    if (FAILED(gpu_->device()->CreateTexture2D(&in, nullptr, &input_)) ||
        FAILED(video_device_->CreateVideoProcessorInputView(input_.Get(), enumerator_.Get(), &in_view, &input_view_))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC out = in;
    out.Width = w;
    out.Height = h;
    out.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    out.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;  // drawn by Direct2D
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_view{};
    out_view.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    for (std::size_t i = 0; i < outputs_.size(); ++i) {
        if (FAILED(gpu_->device()->CreateTexture2D(&out, nullptr, &outputs_[i])) ||
            FAILED(video_device_->CreateVideoProcessorOutputView(outputs_[i].Get(), enumerator_.Get(), &out_view,
                                                                 &output_views_[i]))) {
            return false;
        }
    }

    // BT.709 both ways: limited-range YCbCr in, full-range RGB out. Only the shown part of the
    // picture: the encoder pads 1080 rows to 1088.
    ComPtr<ID3D11VideoContext1> context1;
    if (SUCCEEDED(video_context_.As(&context1))) {
        context1->VideoProcessorSetStreamColorSpace1(processor_.Get(), 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
        context1->VideoProcessorSetOutputColorSpace1(processor_.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv{};
        yuv.YCbCr_Matrix = 1;  // BT.709
        yuv.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{};
        rgb.RGB_Range = 0;  // 0-255
        video_context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &yuv);
        video_context_->VideoProcessorSetOutputColorSpace(processor_.Get(), &rgb);
    }
    const RECT shown{0, 0, width, height};
    video_context_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &shown);
    video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &shown);
    video_context_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &shown);
    input_w_ = decoded.Width;
    input_h_ = decoded.Height;
    width_ = width;
    height_ = height;
    return true;
}

ComPtr<ID3D11Texture2D> PictureConverter::to_texture(const H264Decoder::Picture& picture) {
    if (!video_device_ || !picture.texture || picture.width <= 0 || picture.height <= 0) return nullptr;
    ComPtr<ID3D11Device> device;
    picture.texture->GetDevice(&device);
    if (device.Get() != gpu_->device()) return nullptr;  // decoded on a device that has been replaced

    D3D11_TEXTURE2D_DESC decoded{};
    picture.texture->GetDesc(&decoded);
    const GpuDevice::Lock lock(*gpu_);
    if (!prepare(decoded, picture.width, picture.height)) {
        if (!warned_) log::warn("h264: can't set up the video processor for {}x{}", picture.width, picture.height);
        warned_ = true;
        processor_.Reset();
        return nullptr;
    }
    // A copy of the decoder's slice: its texture goes back to the decoder with the sample.
    gpu_->context()->CopySubresourceRegion(input_.Get(), 0, 0, 0, 0, picture.texture.Get(), picture.subresource,
                                           nullptr);
    const std::size_t slot = next_;
    next_ = (next_ + 1) % outputs_.size();
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view_.Get();
    if (FAILED(video_context_->VideoProcessorBlt(processor_.Get(), output_views_[slot].Get(), 0, 1, &stream))) {
        if (!warned_) log::warn("h264: the picture couldn't be converted to BGRA");
        warned_ = true;
        return nullptr;
    }
    return outputs_[slot];
}

bool PictureConverter::to_bgra(const H264Decoder::Picture& picture, BgraImage& out) {
    const int w = picture.width, h = picture.height;
    const auto stride = static_cast<std::size_t>(picture.stride);
    if (picture.nv12.empty() || w <= 0 || h <= 0 || stride < static_cast<std::size_t>(w) ||
        picture.nv12.size() < stride * static_cast<std::size_t>(picture.coded_height) * 3 / 2) {
        return false;
    }
    out.width = w;
    out.height = h;
    out.pixels.resize(out.stride() * static_cast<std::size_t>(h));
    const std::uint8_t* uv_plane = picture.nv12.data() + stride * static_cast<std::size_t>(picture.coded_height);
    // BT.709 limited range to full-range R'G'B', in 1/1024 steps.
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* luma = picture.nv12.data() + stride * static_cast<std::size_t>(y);
        const std::uint8_t* uv = uv_plane + stride * static_cast<std::size_t>(y / 2);
        std::uint8_t* px = out.pixels.data() + out.stride() * static_cast<std::size_t>(y);
        for (int x = 0; x < w; ++x) {
            const int c = 1192 * (luma[x] - 16);
            const int d = uv[(x / 2) * 2] - 128;
            const int e = uv[(x / 2) * 2 + 1] - 128;
            px[4 * x + 0] = clamp_byte((c + 2163 * d + 512) >> 10);
            px[4 * x + 1] = clamp_byte((c - 218 * d - 546 * e + 512) >> 10);
            px[4 * x + 2] = clamp_byte((c + 1836 * e + 512) >> 10);
            px[4 * x + 3] = 255;
        }
    }
    return true;
}

}  // namespace kvm
