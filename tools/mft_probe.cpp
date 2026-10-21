// SPDX-License-Identifier: Apache-2.0

// mft_probe: which H.264 / HEVC / AV1 encoders and decoders this PC has through Media Foundation
// (roadmap P2.2), and what each can do. The results decide what P2.3 (GPU encode) and P2.10
// (HEVC / AV1 / 4:4:4) target; they go into docs/PERF.md per PC.
//
//   mft_probe.exe            every codec, hardware and software
//   mft_probe.exe --markdown the same as docs/PERF.md table rows
//
// For each transform: hardware or software, the vendor (PCI ID) and graphics adapter, whether it works
// on D3D11 textures (MF_SA_D3D11_AWARE), async (hardware MFTs are), low-latency mode
// (CODECAPI_AVLowLatencyMode), and the raw formats it takes (encoders) or gives (decoders). A 4:4:4
// format there (AYUV, Y410, Y416) says it may do 4:4:4; whether it really encodes 4:4:4 needs P2.10's
// test. RGB input doesn't count: the encoder turns it into 4:2:0 itself.
//
// Hardware decoding usually isn't a vendor MFT: Microsoft's decoder MFT decodes on the GPU (DXVA) when
// it gets a D3D11 device. So the GPU's own decode profiles (ID3D11VideoDevice) are listed too, per
// adapter: H.264, HEVC Main / Main10, VP9, AV1.

#include <windows.h>
#include <codecapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")  // the D3D11_DECODER_PROFILE_* GUIDs

using Microsoft::WRL::ComPtr;

namespace {

struct Codec {
    const char* name;
    GUID subtype;
};
const Codec kCodecs[] = {{"H.264", MFVideoFormat_H264}, {"HEVC", MFVideoFormat_HEVC}, {"AV1", MFVideoFormat_AV1}};

struct RawFormat {
    GUID subtype;
    const char* name;
    bool full_chroma;  // 4:4:4
};
const RawFormat kRawFormats[] = {
    {MFVideoFormat_NV12, "NV12", false}, {MFVideoFormat_YUY2, "YUY2", false},     {MFVideoFormat_IYUV, "IYUV", false},
    {MFVideoFormat_I420, "I420", false}, {MFVideoFormat_YV12, "YV12", false},     {MFVideoFormat_P010, "P010", false},
    {MFVideoFormat_P016, "P016", false}, {MFVideoFormat_ARGB32, "ARGB32", false}, {MFVideoFormat_RGB32, "RGB32", false},
    {MFVideoFormat_AYUV, "AYUV", true},  {MFVideoFormat_Y410, "Y410", true},      {MFVideoFormat_Y416, "Y416", true},
};

std::string narrow(std::wstring_view s) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(len, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

struct Adapter {
    std::string name;
    ComPtr<IDXGIAdapter1> dxgi;
};

std::vector<Adapter> adapters() {
    std::vector<Adapter> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;  // Microsoft Basic Render Driver
        out.push_back({narrow(desc.Description), adapter});
    }
    return out;
}

// The GPU's own decode profiles: what DXVA can decode on it.
std::string decode_profiles(IDXGIAdapter1* adapter) {
    struct Profile {
        GUID id;
        const char* name;
    };
    const Profile known[] = {{D3D11_DECODER_PROFILE_H264_VLD_NOFGT, "H.264"},
                             {D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, "HEVC Main"},
                             {D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10, "HEVC Main10"},
                             {D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0, "VP9"},
                             {D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, "AV1"}};
    ComPtr<ID3D11Device> device;
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr,
                                 0, D3D11_SDK_VERSION, &device, nullptr, nullptr))) {
        return "no D3D11 video device";
    }
    ComPtr<ID3D11VideoDevice> video;
    if (FAILED(device.As(&video))) return "no D3D11 video device";
    std::string out;
    for (const auto& p : known) {
        for (UINT i = 0; i < video->GetVideoDecoderProfileCount(); ++i) {
            GUID id{};
            if (SUCCEEDED(video->GetVideoDecoderProfile(i, &id)) && id == p.id) {
                if (!out.empty()) out += ", ";
                out += p.name;
                break;
            }
        }
    }
    return out.empty() ? "none of H.264 / HEVC / VP9 / AV1" : out;
}

std::string string_attribute(IMFAttributes* attrs, const GUID& key) {
    UINT32 length = 0;
    if (!attrs || FAILED(attrs->GetStringLength(key, &length))) return {};
    std::wstring value(length + 1, L'\0');
    if (FAILED(attrs->GetString(key, value.data(), length + 1, &length))) return {};
    value.resize(length);
    return narrow(value);
}

// "VEN_8086&DEV_..." in a hardware MFT's vendor ID: the GPU maker.
std::string vendor(const std::string& id) {
    if (id.find("8086") != std::string::npos) return "Intel";
    if (id.find("10DE") != std::string::npos || id.find("10de") != std::string::npos) return "NVIDIA";
    if (id.find("1002") != std::string::npos) return "AMD";
    if (id.find("5143") != std::string::npos) return "Qualcomm";
    return id.empty() ? "-" : id;
}

// The raw formats the MFT lists for its input (encoder) or output (decoder) at registration.
std::string raw_formats(IMFActivate* activate, bool encoder, bool& full_chroma) {
    UINT8* blob = nullptr;
    UINT32 size = 0;
    const GUID key = encoder ? MFT_INPUT_TYPES_Attributes : MFT_OUTPUT_TYPES_Attributes;
    std::string out;
    full_chroma = false;
    if (FAILED(activate->GetAllocatedBlob(key, &blob, &size))) return "-";
    const auto* types = reinterpret_cast<const MFT_REGISTER_TYPE_INFO*>(blob);
    for (UINT32 i = 0; i < size / sizeof(MFT_REGISTER_TYPE_INFO); ++i) {
        for (const auto& f : kRawFormats) {
            if (types[i].guidSubtype != f.subtype) continue;
            if (!out.empty()) out += ' ';
            out += f.name;
            full_chroma = full_chroma || f.full_chroma;
        }
    }
    CoTaskMemFree(blob);
    return out.empty() ? "-" : out;
}

struct Found {
    std::string codec;
    bool encoder = false;
    std::string name;
    bool hardware = false;
    std::string vendor;
    std::string d3d11 = "?";
    std::string async_mft = "?";
    std::string low_latency = "?";
    std::string formats;
    bool full_chroma = false;
};

std::vector<Found> probe(const Codec& codec, bool encoder) {
    std::vector<Found> found;
    const MFT_REGISTER_TYPE_INFO type{MFMediaType_Video, codec.subtype};
    const UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT |
                         MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;
    IMFActivate** list = nullptr;
    UINT32 count = 0;
    const HRESULT hr = MFTEnumEx(encoder ? MFT_CATEGORY_VIDEO_ENCODER : MFT_CATEGORY_VIDEO_DECODER, flags,
                                 encoder ? nullptr : &type, encoder ? &type : nullptr, &list, &count);
    if (FAILED(hr)) return found;
    for (UINT32 i = 0; i < count; ++i) {
        ComPtr<IMFActivate> activate;
        activate.Attach(list[i]);
        Found f{.codec = codec.name, .encoder = encoder};
        f.name = string_attribute(activate.Get(), MFT_FRIENDLY_NAME_Attribute);
        UINT32 mft_flags = 0;
        activate->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &mft_flags);
        f.hardware = (mft_flags & MFT_ENUM_FLAG_HARDWARE) != 0;
        f.vendor = f.hardware ? vendor(string_attribute(activate.Get(), MFT_ENUM_HARDWARE_VENDOR_ID_Attribute)) : "-";
        f.formats = raw_formats(activate.Get(), encoder, f.full_chroma);

        // What only the transform itself can say.
        ComPtr<IMFTransform> transform;
        if (SUCCEEDED(activate->ActivateObject(IID_PPV_ARGS(&transform)))) {
            ComPtr<IMFAttributes> attrs;
            if (SUCCEEDED(transform->GetAttributes(&attrs)) && attrs) {
                UINT32 value = 0;
                f.d3d11 = SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &value)) && value ? "yes" : "no";
                value = 0;
                f.async_mft = SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &value)) && value ? "yes" : "no";
            }
            ComPtr<ICodecAPI> codec_api;
            if (SUCCEEDED(transform.As(&codec_api))) {
                f.low_latency = codec_api->IsSupported(&CODECAPI_AVLowLatencyMode) == S_OK ? "yes" : "no";
            } else {
                f.low_latency = "no API";
            }
            transform.Reset();
            activate->ShutdownObject();
        } else {
            f.d3d11 = f.async_mft = f.low_latency = "can't open";
        }
        found.push_back(std::move(f));
    }
    CoTaskMemFree(list);
    return found;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const bool markdown = argc > 1 && std::wstring_view(argv[1]) == L"--markdown";
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        std::printf(
            "Media Foundation isn't available here (an N edition of Windows without the Media Feature Pack?)\n");
        return 1;
    }
    const auto gpus = adapters();
    std::vector<Found> all;
    for (const auto& codec : kCodecs) {
        for (const bool encoder : {true, false}) {
            auto found = probe(codec, encoder);
            all.insert(all.end(), found.begin(), found.end());
        }
    }
    // Windows lists a transform once per adapter it serves, and a GPU once per adapter it backs (a
    // virtual display adds some): identical lines are printed once, with how many there were.
    std::vector<std::pair<std::string, int>> lines;
    const auto add = [&](std::string line) {
        for (auto& [text, n] : lines) {
            if (text == line) {
                ++n;
                return;
            }
        }
        lines.emplace_back(std::move(line), 1);
    };
    const auto flush = [&] {
        for (const auto& [text, n] : lines) {
            // A table row must end with its last "|": the count goes into the plain listing only.
            std::printf("%s%s\n", text.c_str(), n > 1 && !markdown ? std::format("  (x{})", n).c_str() : "");
        }
        lines.clear();
    };

    if (markdown) {
        std::printf(
            "| Codec | Kind | Name | Hardware | Vendor | D3D11 | Async | Low latency | Raw formats | 4:4:4 "
            "format |\n");
        std::printf("|---|---|---|---|---|---|---|---|---|---|\n");
    }
    for (const auto& f : all) {
        const char* kind = f.encoder ? "encoder" : "decoder";
        add(markdown ? std::format("| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |", f.codec, kind, f.name,
                                   f.hardware ? "yes" : "no", f.vendor, f.d3d11, f.async_mft, f.low_latency, f.formats,
                                   f.full_chroma ? "listed" : "-")
                     : std::format("{:5} {:7} {:<45} {:8} {:7} d3d11 {:<4} async {:<4} low-latency {:<4} {}", f.codec,
                                   kind, f.name, f.hardware ? "hardware" : "software", f.vendor, f.d3d11, f.async_mft,
                                   f.low_latency, f.formats));
    }
    flush();
    if (all.empty()) std::printf("no H.264, HEVC or AV1 transforms found\n");

    // What each GPU decodes itself (DXVA): Microsoft's decoder MFTs use it when given a D3D11 device.
    std::printf(markdown ? "\n| GPU | Decodes in hardware (D3D11 video) |\n|---|---|\n"
                         : "\nGPU decode (D3D11 video):\n");
    for (const auto& gpu : gpus) {
        const std::string profiles = decode_profiles(gpu.dxgi.Get());
        add(markdown ? std::format("| {} | {} |", gpu.name, profiles) : std::format("  {:<40} {}", gpu.name, profiles));
    }
    flush();
    MFShutdown();
    CoUninitialize();
    return 0;
}
