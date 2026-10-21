// SPDX-License-Identifier: Apache-2.0

#include "nv12_converter.h"

#include <d3d11_1.h>

#include <utility>

#include "kvm/log.h"

namespace kvm {

namespace {

using Microsoft::WRL::ComPtr;

}  // namespace

Nv12Converter::Nv12Converter(std::shared_ptr<GpuDevice> gpu) : gpu_(std::move(gpu)) {
    if (!gpu_ || FAILED(gpu_->device()->QueryInterface(IID_PPV_ARGS(&video_device_))) ||
        FAILED(gpu_->context()->QueryInterface(IID_PPV_ARGS(&video_context_)))) {
        log::warn("nv12: this device has no video processor");
    }
}

bool Nv12Converter::prepare(int width, int height) {
    if (processor_ && width == width_ && height == height_) return true;
    processor_.Reset();
    enumerator_.Reset();
    input_.Reset();
    input_view_.Reset();
    output_.Reset();
    output_view_.Reset();
    width_ = height_ = 0;

    const auto w = static_cast<UINT>(width), h = static_cast<UINT>(height);
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = content.OutputWidth = w;
    content.InputHeight = content.OutputHeight = h;
    content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(video_device_->CreateVideoProcessorEnumerator(&content, &enumerator_)) ||
        FAILED(video_device_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_))) {
        return false;
    }

    D3D11_TEXTURE2D_DESC in{};
    in.Width = w;
    in.Height = h;
    in.MipLevels = 1;
    in.ArraySize = 1;
    in.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    in.SampleDesc.Count = 1;
    in.Usage = D3D11_USAGE_DEFAULT;
    in.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_TEXTURE2D_DESC out = in;
    out.Format = DXGI_FORMAT_NV12;
    out.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(gpu_->device()->CreateTexture2D(&in, nullptr, &input_)) ||
        FAILED(gpu_->device()->CreateTexture2D(&out, nullptr, &output_))) {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_view{};
    in_view.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_view{};
    out_view.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (FAILED(video_device_->CreateVideoProcessorInputView(input_.Get(), enumerator_.Get(), &in_view, &input_view_)) ||
        FAILED(video_device_->CreateVideoProcessorOutputView(output_.Get(), enumerator_.Get(), &out_view,
                                                             &output_view_))) {
        return false;
    }

    // BT.709 both ways, full-range RGB in, limited-range YCbCr out.
    ComPtr<ID3D11VideoContext1> context1;
    if (SUCCEEDED(video_context_.As(&context1))) {
        context1->VideoProcessorSetStreamColorSpace1(processor_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        context1->VideoProcessorSetOutputColorSpace1(processor_.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{};
        rgb.RGB_Range = 0;  // 0-255
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv{};
        yuv.YCbCr_Matrix = 1;  // BT.709
        yuv.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        video_context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &rgb);
        video_context_->VideoProcessorSetOutputColorSpace(processor_.Get(), &yuv);
    }
    video_context_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    width_ = width;
    height_ = height;
    return true;
}

ID3D11Texture2D* Nv12Converter::convert(ID3D11Texture2D* bgra, int width, int height) {
    width &= ~1;
    height &= ~1;
    if (!video_device_ || !bgra || width <= 0 || height <= 0) return nullptr;
    const GpuDevice::Lock lock(*gpu_);
    if (!prepare(width, height)) {
        if (!warned_) log::warn("nv12: can't set up the video processor for {}x{}", width, height);
        warned_ = true;
        processor_.Reset();
        return nullptr;
    }
    // Into a texture the video processor can read: WGC's own textures may not allow an input view,
    // and they can be larger than the picture right after a resize.
    const D3D11_BOX box{0, 0, 0, static_cast<UINT>(width), static_cast<UINT>(height), 1};
    gpu_->context()->CopySubresourceRegion(input_.Get(), 0, 0, 0, 0, bgra, 0, &box);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view_.Get();
    if (FAILED(video_context_->VideoProcessorBlt(processor_.Get(), output_view_.Get(), 0, 1, &stream))) {
        if (!warned_) log::warn("nv12: the conversion failed");
        warned_ = true;
        return nullptr;
    }
    return output_.Get();
}

}  // namespace kvm
