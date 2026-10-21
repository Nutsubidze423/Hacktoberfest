// SPDX-License-Identifier: Apache-2.0

#include "h264_encoder.h"

#include <codecapi.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>

#include <format>
#include <utility>

#include "kvm/log.h"
#include "kvm/mf_util.h"
#include "kvm/net.h"  // steady_now_us

namespace kvm {

namespace {

using mf::hr_text;
using Microsoft::WRL::ComPtr;

// The hardware H.264 encoders on the adapter `device` runs on (MFTEnum2 can filter by adapter; an
// encoder on another GPU couldn't read our textures).
std::vector<ComPtr<IMFActivate>> hardware_encoders(ID3D11Device* device) {
    std::vector<ComPtr<IMFActivate>> out;
    ComPtr<IMFAttributes> filter;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(MFCreateAttributes(&filter, 1)) && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
        SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc))) {
        const auto luid =
            static_cast<UINT64>(static_cast<UINT32>(desc.AdapterLuid.HighPart)) << 32 | desc.AdapterLuid.LowPart;
        filter->SetUINT64(MFT_ENUM_ADAPTER_LUID, luid);
    }
    const MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12};
    const MFT_REGISTER_TYPE_INFO h264{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** list = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in, &h264,
                        filter.Get(), &list, &count))) {
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

bool H264Encoder::available(const GpuDevice& gpu) {
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
    const bool found = !hardware_encoders(gpu.device()).empty();
    MFShutdown();
    return found;
}

std::unique_ptr<H264Encoder> H264Encoder::create(std::shared_ptr<GpuDevice> gpu, const Options& options,
                                                 std::string& why) {
    if (!gpu || options.width <= 0 || options.height <= 0 || options.width % 2 || options.height % 2) {
        why = "h264: needs a GPU and an even, non-zero size";
        return nullptr;
    }
    auto enc = std::unique_ptr<H264Encoder>(new H264Encoder());
    enc->gpu_ = std::move(gpu);
    enc->options_ = options;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        why = "h264: Media Foundation isn't available";
        return nullptr;
    }
    enc->started_mf_ = true;

    const auto encoders = hardware_encoders(enc->gpu_->device());
    if (encoders.empty()) {
        why = "h264: no hardware H.264 encoder on " + enc->gpu_->adapter_name();
        return nullptr;
    }
    enc->activate_ = encoders.front();
    enc->name_ = mf::friendly_name(enc->activate_.Get(), "H.264 encoder");
    HRESULT hr = enc->activate_->ActivateObject(IID_PPV_ARGS(&enc->transform_));
    if (FAILED(hr)) {
        why = std::format("h264: {} won't start ({})", enc->name_, hr_text(hr));
        return nullptr;
    }

    // Hardware encoders are async MFTs: they must be unlocked before use, and then talk through events.
    ComPtr<IMFAttributes> attrs;
    UINT32 async = 0;
    if (SUCCEEDED(enc->transform_->GetAttributes(&attrs)) && SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async)) &&
        async) {
        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    }
    if (!async || FAILED(enc->transform_.As(&enc->events_))) {
        why = "h264: " + enc->name_ + " isn't an async MFT";
        return nullptr;
    }
    if (attrs) attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

    // The same D3D11 device as capture and NV12: textures go in without a copy to the CPU.
    UINT token = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&token, &enc->manager_)) ||
        FAILED(enc->manager_->ResetDevice(enc->gpu_->device(), token)) ||
        FAILED(enc->transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                               reinterpret_cast<ULONG_PTR>(enc->manager_.Get())))) {
        why = "h264: " + enc->name_ + " doesn't take the D3D11 device";
        return nullptr;
    }

    // Low latency, no B-frames, a long GOP, constant bitrate. Set before the media types: some
    // encoders read them only then. Not every encoder knows every setting; that isn't fatal.
    enc->transform_.As(&enc->codec_);
    enc->set_codec_value(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    enc->set_codec_value(CODECAPI_AVEncCommonMeanBitRate, static_cast<std::uint32_t>(options.bitrate));
    enc->set_codec_value(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    enc->set_codec_value(CODECAPI_AVEncMPVGOPSize, static_cast<std::uint32_t>(options.fps) *
                                                       static_cast<std::uint32_t>(options.keyframe_interval_s));
    if (enc->codec_) {
        VARIANT v{};
        v.vt = VT_BOOL;
        v.boolVal = VARIANT_TRUE;
        enc->codec_->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }

    const auto w = static_cast<UINT32>(options.width), h = static_cast<UINT32>(options.height);
    ComPtr<IMFMediaType> out_type;
    MFCreateMediaType(&out_type);
    out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out_type->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(options.bitrate));
    out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(options.fps), 1);
    MFSetAttributeRatio(out_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = enc->transform_->SetOutputType(0, out_type.Get(), 0);
    if (FAILED(hr)) {
        why = std::format("h264: {} won't encode {}x{} at {} fps ({})", enc->name_, w, h, options.fps, hr_text(hr));
        return nullptr;
    }
    ComPtr<IMFMediaType> in_type;
    MFCreateMediaType(&in_type);
    in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(options.fps), 1);
    MFSetAttributeRatio(in_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = enc->transform_->SetInputType(0, in_type.Get(), 0);
    if (FAILED(hr)) {
        why = std::format("h264: {} won't take NV12 input ({})", enc->name_, hr_text(hr));
        return nullptr;
    }

    MFT_OUTPUT_STREAM_INFO info{};
    if (SUCCEEDED(enc->transform_->GetOutputStreamInfo(0, &info))) {
        enc->provides_samples_ = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        enc->output_size_ = info.cbSize;
    }
    enc->transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    enc->transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    // The stream starts with a keyframe anyway; forcing one before the first picture is not needed.
    enc->thread_ = std::thread([e = enc.get()] { e->event_loop(); });
    log::info("h264: {} on {}, {}x{} at {} fps, {:.1f} Mbit/s", enc->name_, enc->gpu_->adapter_name(), w, h,
              options.fps, options.bitrate / 1e6);
    return enc;
}

H264Encoder::~H264Encoder() {
    stopping_ = true;
    if (transform_) {
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    // Shutting the transform down ends GetEvent with MF_E_SHUTDOWN: the event thread returns.
    ComPtr<IMFShutdown> shutdown;
    if (transform_ && SUCCEEDED(transform_.As(&shutdown))) shutdown->Shutdown();
    if (activate_) activate_->ShutdownObject();
    if (thread_.joinable()) thread_.join();
    packets_.close();
    waiting_.clear();
    codec_.Reset();
    events_.Reset();
    transform_.Reset();
    manager_.Reset();
    activate_.Reset();
    if (started_mf_) MFShutdown();
}

bool H264Encoder::set_codec_value(const GUID& key, std::uint32_t value) {
    if (!codec_) return false;
    VARIANT v{};
    v.vt = VT_UI4;
    v.ulVal = value;
    return SUCCEEDED(codec_->SetValue(&key, &v));
}

void H264Encoder::set_bitrate(int bits_per_second) {
    std::lock_guard lock(mutex_);
    options_.bitrate = bits_per_second;
    set_codec_value(CODECAPI_AVEncCommonMeanBitRate, static_cast<std::uint32_t>(bits_per_second));
}

bool H264Encoder::encode(ID3D11Texture2D* nv12, std::int64_t t_capture_us) {
    if (failed_ || !nv12) return false;
    // A texture of our own for this picture: the converter re-uses its output, and the encoder may
    // still be reading the previous one.
    D3D11_TEXTURE2D_DESC desc{};
    nv12->GetDesc(&desc);
    desc.Width = static_cast<UINT>(options_.width);
    desc.Height = static_cast<UINT>(options_.height);
    desc.BindFlags = D3D11_BIND_VIDEO_ENCODER;  // the hardware encoder reads it directly
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> copy;
    if (FAILED(gpu_->device()->CreateTexture2D(&desc, nullptr, &copy))) return false;
    {
        const GpuDevice::Lock lock(*gpu_);
        const D3D11_BOX box{0, 0, 0, desc.Width, desc.Height, 1};
        gpu_->context()->CopySubresourceRegion(copy.Get(), 0, 0, 0, 0, nv12, 0, &box);
    }
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), copy.Get(), 0, FALSE, &buffer)) ||
        FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(buffer.Get()))) {
        return false;
    }
    // Sample times are in 100 ns, counted from the first picture (an encoder's rate control expects
    // times near 0); the packet maps it back to the capture time.
    std::int64_t base = -1;
    time_base_us_.compare_exchange_strong(base, t_capture_us);
    sample->SetSampleTime((t_capture_us - time_base_us_) * 10);
    sample->SetSampleDuration(10'000'000 / options_.fps);

    const bool keyframe = keyframe_requested_.exchange(false);
    std::lock_guard lock(mutex_);
    if (waiting_.size() >= 2) {  // behind: the newest picture wins, and takes over a keyframe request
        const bool was_keyframe = waiting_.front().keyframe;
        waiting_.pop_front();
        if (was_keyframe) waiting_.front().keyframe = true;
        ++dropped_;
    }
    waiting_.push_back({std::move(sample), keyframe});
    feed_locked();
    return !failed_;
}

void H264Encoder::feed_locked() {
    while (need_input_ > 0 && !waiting_.empty() && !failed_) {
        Waiting next = std::move(waiting_.front());
        waiting_.pop_front();
        if (next.keyframe) set_codec_value(CODECAPI_AVEncVideoForceKeyFrame, 1);
        const HRESULT hr = transform_->ProcessInput(0, next.sample.Get(), 0);
        --need_input_;
        if (FAILED(hr)) {
            fail("h264: the encoder refused a picture (" + hr_text(hr) + ")");
            return;
        }
        ++in_flight_;
    }
}

void H264Encoder::drain_output() {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER out{};
        ComPtr<IMFSample> own;
        if (!provides_samples_) {
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(MFCreateSample(&own)) || FAILED(MFCreateMemoryBuffer(output_size_, &buffer)) ||
                FAILED(own->AddBuffer(buffer.Get()))) {
                fail("h264: out of memory for the output");
                return;
            }
            out.pSample = own.Get();
        }
        DWORD status = 0;
        HRESULT hr;
        {
            std::lock_guard lock(mutex_);
            hr = transform_->ProcessOutput(0, 1, &out, &status);
        }
        if (out.pEvents) out.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
        // "No sample this time": Intel's encoder answers its first METransformHaveOutput like this (with
        // E_UNEXPECTED). Nothing came out; the picture comes with a later event.
        if ((out.dwStatus & MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE) == MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE) return;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // The encoder changed its output type (e.g. its own stride): take the one it offers.
            ComPtr<IMFMediaType> type;
            std::lock_guard lock(mutex_);
            if (FAILED(transform_->GetOutputAvailableType(0, 0, &type)) ||
                FAILED(transform_->SetOutputType(0, type.Get(), 0))) {
                fail("h264: the encoder changed its output and can't go on");
                return;
            }
            continue;
        }
        if (FAILED(hr)) {
            fail(std::format("h264: no output from the encoder ({})", hr_text(hr)));
            return;
        }
        ComPtr<IMFSample> sample;
        sample.Attach(provides_samples_ ? out.pSample : own.Detach());
        Packet packet;
        LONGLONG time = 0;
        if (SUCCEEDED(sample->GetSampleTime(&time))) packet.t_capture_us = time / 10 + time_base_us_;
        packet.t_encoded_us = steady_now_us();
        packet.keyframe = MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, FALSE) != FALSE;
        ComPtr<IMFMediaBuffer> buffer;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
            packet.annexb.assign(data, data + length);
            buffer->Unlock();
        }
        // Queued before it counts as out: flush() promises the packet is in packets() when it returns.
        if (packet.annexb.empty()) {
            if (empty_++ == 0) log::warn("h264: the encoder put out an empty sample (picture skipped)");
        } else {
            packets_.push(std::move(packet));
        }
        {
            std::lock_guard lock(mutex_);
            if (in_flight_ > 0) --in_flight_;
            if (in_flight_ == 0 && waiting_.empty()) flushed_.notify_all();
        }
        return;  // one output per METransformHaveOutput
    }
}

void H264Encoder::event_loop() {
    // The encoder's outputs are taken on this thread: give it a COM apartment, as Media Foundation expects.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!stopping_ && !failed_) {
        ComPtr<IMFMediaEvent> event;
        const HRESULT hr = events_->GetEvent(0, &event);  // blocks; MF_E_SHUTDOWN when we stop
        if (FAILED(hr)) {
            if (!stopping_) fail("h264: the encoder stopped (" + hr_text(hr) + ")");
            break;
        }
        MediaEventType type = MEUnknown;
        event->GetType(&type);
        if (type == METransformNeedInput) {
            std::lock_guard lock(mutex_);
            ++need_input_;
            feed_locked();
        } else if (type == METransformHaveOutput) {
            drain_output();
        }
    }
    packets_.close();
    {
        std::lock_guard lock(mutex_);
        flushed_.notify_all();
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

bool H264Encoder::flush(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return flushed_.wait_for(lock, timeout, [&] { return failed_ || (in_flight_ == 0 && waiting_.empty()); }) &&
           !failed_;
}

void H264Encoder::fail(const std::string& why) {
    if (!failed_.exchange(true)) log::error("{}", why);
    packets_.close();
}

}  // namespace kvm
