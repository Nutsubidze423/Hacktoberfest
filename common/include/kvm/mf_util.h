// SPDX-License-Identifier: Apache-2.0

// Small helpers shared by the Media Foundation H.264 encoder (host_a/h264_encoder.h) and decoder
// (client_b/h264_decoder.h). Header-only: kvm_common doesn't link Media Foundation, the modules do.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfobjects.h>
#include <mftransform.h>

#include <cstdint>
#include <format>
#include <string>

namespace kvm::mf {

inline std::string hr_text(HRESULT hr) {
    return std::format("0x{:08X}", static_cast<std::uint32_t>(hr));
}

// An MFT's friendly name (UTF-8), e.g. "Microsoft H264 Video Decoder MFT", or `fallback`.
inline std::string friendly_name(IMFActivate* activate, const char* fallback) {
    UINT32 length = 0;
    if (FAILED(activate->GetStringLength(MFT_FRIENDLY_NAME_Attribute, &length))) return fallback;
    std::wstring name(length + 1, L'\0');
    if (FAILED(activate->GetString(MFT_FRIENDLY_NAME_Attribute, name.data(), length + 1, &length))) return fallback;
    const int n = static_cast<int>(length);
    const int len = WideCharToMultiByte(CP_UTF8, 0, name.data(), n, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len > 0 ? len : 0), '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, name.data(), n, out.data(), len, nullptr, nullptr);
    return out;
}

}  // namespace kvm::mf
