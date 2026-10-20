// SPDX-License-Identifier: Apache-2.0

#include "jpeg_encoder.h"

#include <turbojpeg.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include "kvm/log.h"

namespace kvm {

JpegEncoder::JpegEncoder(int quality, bool full_chroma)
    : handle_(tj3Init(TJINIT_COMPRESS)), quality_(std::clamp(quality, 1, 100)), subsamp_(full_chroma ? TJSAMP_444 : TJSAMP_420) {
    if (!handle_) throw std::runtime_error("jpeg: tj3Init failed");
    tj3Set(handle_, TJPARAM_QUALITY, quality_);
    tj3Set(handle_, TJPARAM_SUBSAMP, subsamp_);
    tj3Set(handle_, TJPARAM_FASTDCT, 1);    // faster; the difference is invisible at quality 70+
    tj3Set(handle_, TJPARAM_NOREALLOC, 1);  // encode straight into the caller's buffer
}

JpegEncoder::~JpegEncoder() {
    tj3Destroy(handle_);
}

bool JpegEncoder::encode(const CapturedFrame& frame, std::vector<std::uint8_t>& out) {
    out.clear();
    if (frame.width <= 0 || frame.height <= 0 || frame.stride < frame.width * 4 ||
        frame.bgra.size() < static_cast<size_t>(frame.stride) * static_cast<size_t>(frame.height)) {
        log::error("jpeg: bad frame {}x{} stride {} ({} bytes)", frame.width, frame.height, frame.stride,
                   frame.bgra.size());
        return false;
    }
    return encode(frame.bgra.data(), frame.width, frame.height, frame.stride, out);
}

bool JpegEncoder::encode(const std::uint8_t* first, int width, int height, int stride, std::vector<std::uint8_t>& out) {
    out.clear();
    if (!first || width <= 0 || height <= 0 || stride < width * 4) {
        log::error("jpeg: bad image {}x{} stride {}", width, height, stride);
        return false;
    }

    const auto start = std::chrono::steady_clock::now();
    // Worst-case JPEG size, so TurboJPEG never needs to reallocate.
    out.resize(tj3JPEGBufSize(width, height, subsamp_));
    unsigned char* dst = out.data();
    size_t size = out.size();
    if (tj3Compress8(handle_, first, width, stride, height, TJPF_BGRX, &dst, &size) != 0) {
        log::error("jpeg: {}", tj3GetErrorStr(handle_));
        out.clear();
        return false;
    }
    out.resize(size);
    last_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return true;
}

}  // namespace kvm
