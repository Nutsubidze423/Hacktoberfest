// SPDX-License-Identifier: Apache-2.0

#include "h264_decoder.h"

#include <mfapi.h>
#include <mferror.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <thread>
#include <utility>

#include "kvm/log.h"
#include "kvm/mf_util.h"

namespace kvm {

namespace {

using mf::hr_text;
using Microsoft::WRL::ComPtr;

// The synchronous H.264 decoders Media Foundation has (Microsoft's own comes first). Given a D3D11
// device it decodes through DXVA; hardware vendors don't ship decoder MFTs of their own for this.
std::vector<ComPtr<IMFActivate>> decoders() {
    std::vector<ComPtr<IMFActivate>> out;
    const MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_H264};
    const MFT_REGISTER_TYPE_INFO nv12{MFMediaType_Video, MFVideoFormat_NV12};
    IMFActivate** list = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                         MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, &nv12,
                         &list, &count))) {
        return out;
    }
    for (UINT32 i = 0; i < count; ++i) {
        ComPtr<IMFActivate> a;
        a.Attach(list[i]);
        out.push_back(std::move(a));
    }
    CoTaskMemFree(list);
    return out;
}

}  // namespace

std::unique_ptr<H264Decoder> H264Decoder::create(std::shared_ptr<GpuDevice> gpu, std::string& why) {
    auto dec = std::unique_ptr<H264Decoder>(new H264Decoder());
    dec->gpu_ = std::move(gpu);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        why = "h264: Media Foundation isn't available";
        return nullptr;
    }
    dec->started_mf_ = true;

    for (auto& activate : decoders()) {
        if (SUCCEEDED(activate->ActivateObject(IID_PPV_ARGS(&dec->transform_)))) {
            dec->activate_ = activate;
            dec->name_ = mf::friendly_name(activate.Get(), "H.264 decoder");
            break;
        }
    }
    if (!dec->transform_) {
        why = "h264: Media Foundation has no H.264 decoder here";
        return nullptr;
    }

    ComPtr<IMFAttributes> attrs;
    dec->transform_->GetAttributes(&attrs);
    // A picture out for every access unit in, none held back (CODECAPI_AVLowLatencyMode).
    if (attrs) attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

    // On the GPU: the decoder gets the viewer's D3D11 device and hands out textures on it.
    UINT32 d3d11_aware = 0;
    if (dec->gpu_ && attrs && SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_aware)) && d3d11_aware) {
        UINT token = 0;
        if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &dec->manager_)) &&
            SUCCEEDED(dec->manager_->ResetDevice(dec->gpu_->device(), token)) &&
            SUCCEEDED(dec->transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                                      reinterpret_cast<ULONG_PTR>(dec->manager_.Get())))) {
            dec->on_gpu_ = true;
        } else {
            dec->manager_.Reset();
        }
    }
    if (dec->gpu_ && !dec->on_gpu_)
        log::warn("h264: {} can't decode on the GPU here: decoding in software", dec->name_);

    // The input is H.264; its size comes with the first SPS (the decoder then asks for a new output type).
    ComPtr<IMFMediaType> in_type;
    MFCreateMediaType(&in_type);
    in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    const HRESULT hr = dec->transform_->SetInputType(0, in_type.Get(), 0);
    if (FAILED(hr)) {
        why = std::format("h264: {} won't take H.264 ({})", dec->name_, hr_text(hr));
        return nullptr;
    }
    if (!dec->set_output_type()) {
        why = "h264: " + dec->name_ + " doesn't put out NV12";
        return nullptr;
    }
    dec->transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    dec->transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    log::info("h264: {} ({})", dec->name_,
              dec->on_gpu_ ? "on " + dec->gpu_->adapter_name() : std::string("in software"));
    return dec;
}

H264Decoder::~H264Decoder() {
    if (transform_) transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    transform_.Reset();
    if (activate_) activate_->ShutdownObject();
    activate_.Reset();
    manager_.Reset();
    if (started_mf_) MFShutdown();
}

// NV12 out, at the size the decoder now knows (from the stream's SPS).
bool H264Decoder::set_output_type() {
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        if (FAILED(transform_->GetOutputAvailableType(0, i, &type))) return false;
        GUID subtype{};
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12) continue;
        if (FAILED(transform_->SetOutputType(0, type.Get(), 0))) return false;

        UINT32 w = 0, h = 0;
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
        coded_width_ = static_cast<int>(w);
        coded_height_ = static_cast<int>(h);
        // What to show: the stream's cropping (1080 of 1088 rows), else the whole picture.
        MFVideoArea area{};
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area),
                                    nullptr)) &&
            area.Area.cx > 0 && area.Area.cy > 0) {
            width_ = std::min(static_cast<int>(area.Area.cx), coded_width_);
            height_ = std::min(static_cast<int>(area.Area.cy), coded_height_);
        } else {
            width_ = coded_width_;
            height_ = coded_height_;
        }
        stride_ = static_cast<int>(MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, w));

        MFT_OUTPUT_STREAM_INFO info{};
        if (SUCCEEDED(transform_->GetOutputStreamInfo(0, &info))) {
            provides_samples_ =
                (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            output_size_ = info.cbSize;
        }
        return true;
    }
}

std::optional<H264Decoder::Picture> H264Decoder::decode(std::span<const std::uint8_t> access_unit,
                                                        std::int64_t t_capture_us) {
    std::optional<Picture> newest;
    if (access_unit.empty()) return newest;
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    BYTE* data = nullptr;
    const auto size = static_cast<DWORD>(access_unit.size());
    if (FAILED(MFCreateMemoryBuffer(size, &buffer)) || FAILED(buffer->Lock(&data, nullptr, nullptr))) return newest;
    std::memcpy(data, access_unit.data(), access_unit.size());
    buffer->Unlock();
    buffer->SetCurrentLength(size);
    if (FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(buffer.Get()))) return newest;
    // The capture time rides along as the sample time (100 ns units) and comes out with the picture.
    sample->SetSampleTime(t_capture_us * 10);

    HRESULT hr = transform_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        // Pictures are still waiting to come out: take them first.
        if (take_pictures(newest)) hr = transform_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) {
        if (errors_++ == 0) log::warn("h264: the decoder refused an access unit ({})", hr_text(hr));
        return newest;
    }
    // Low-latency mode holds nothing back, but the decoder decodes on threads of its own: now and then
    // (in software, about one access unit in 100 here) its picture isn't done yet when asked for.
    // Every access unit holds a picture (kvm/annexb.h) and nothing newer can be shown, so wait for it,
    // briefly: otherwise a still screen's last picture would stay inside the decoder.
    const auto started = std::chrono::steady_clock::now();
    while (take_pictures(newest) && (!newest || newest->t_capture_us != t_capture_us)) {
        const auto waited = std::chrono::steady_clock::now() - started;
        if (waited > kPictureWait) {
            ++late_;
            break;
        }
        if (waited < std::chrono::milliseconds(2)) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return newest;
}

// Takes every picture that is ready. False on an error.
bool H264Decoder::take_pictures(std::optional<Picture>& newest) {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER out{};
        ComPtr<IMFSample> own;
        if (!provides_samples_) {
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(MFCreateSample(&own)) || FAILED(MFCreateMemoryBuffer(output_size_, &buffer)) ||
                FAILED(own->AddBuffer(buffer.Get()))) {
                return false;
            }
            out.pSample = own.Get();
        }
        DWORD status = 0;
        const HRESULT hr = transform_->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // The first SPS (or a new one): the picture size is known now.
            if (!set_output_type()) {
                if (errors_++ == 0) log::warn("h264: the decoder changed its output and can't go on");
                return false;
            }
            continue;
        }
        if (FAILED(hr)) {
            if (errors_++ == 0) log::warn("h264: no picture from the decoder ({})", hr_text(hr));
            return false;
        }
        ComPtr<IMFSample> got;
        if (provides_samples_) {
            got.Attach(out.pSample);
        } else {
            got = std::move(own);
        }
        if (!got) return true;
        if (auto picture = picture_from(std::move(got))) {
            if (newest) ++skipped_;
            newest = std::move(picture);
        }
    }
}

std::optional<H264Decoder::Picture> H264Decoder::picture_from(ComPtr<IMFSample> sample) {
    Picture p;
    LONGLONG time = 0;
    if (SUCCEEDED(sample->GetSampleTime(&time))) p.t_capture_us = time / 10;
    p.width = width_;
    p.height = height_;
    p.coded_height = coded_height_;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer))) return std::nullopt;

    // On the GPU: the texture (and slice) the decoder wrote; the sample keeps it ours until released.
    ComPtr<IMFDXGIBuffer> dxgi;
    if (SUCCEEDED(buffer.As(&dxgi)) && SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(&p.texture)))) {
        dxgi->GetSubresourceIndex(&p.subresource);
        p.sample = std::move(sample);
        on_gpu_ = true;
        return p;
    }

    // In memory: copy it out, so the decoder can have its buffer back.
    on_gpu_ = false;
    const auto rows = static_cast<std::size_t>(coded_height_) * 3 / 2;
    ComPtr<IMF2DBuffer> two_d;
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    if (SUCCEEDED(buffer.As(&two_d)) && SUCCEEDED(two_d->Lock2D(&scan0, &pitch))) {
        if (pitch > 0) {
            p.stride = static_cast<int>(pitch);
            p.nv12.assign(scan0, scan0 + rows * static_cast<std::size_t>(pitch));
        }
        two_d->Unlock2D();
    } else {
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length))) return std::nullopt;
        p.stride = stride_ > 0 ? stride_ : coded_width_;
        const std::size_t want = rows * static_cast<std::size_t>(p.stride);
        if (length >= want) p.nv12.assign(data, data + want);
        buffer->Unlock();
    }
    if (p.nv12.empty()) return std::nullopt;
    return p;
}

}  // namespace kvm
