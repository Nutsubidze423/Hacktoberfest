// SPDX-License-Identifier: Apache-2.0

#include "jpeg_decoder.h"

#include <turbojpeg.h>

namespace kvm {

namespace {

// Anything larger is almost certainly a corrupt header, not a real window.
constexpr int kMaxDimension = 16384;

}  // namespace

void JpegDecoder::HandleDeleter::operator()(void* handle) const {
    tj3Destroy(static_cast<tjhandle>(handle));
}

JpegDecoder::JpegDecoder() : handle_(tj3Init(TJINIT_DECOMPRESS)) {
    if (!handle_) {
        error_ = std::string("tj3Init failed: ") + tj3GetErrorStr(nullptr);
    }
}

bool JpegDecoder::decode(std::span<const uint8_t> jpeg, BgraImage& out) {
    error_.clear();
    if (!handle_) {
        error_ = "decoder not initialized";
        return false;
    }
    if (jpeg.empty()) {
        error_ = "empty input";
        return false;
    }

    auto* tj = static_cast<tjhandle>(handle_.get());

    if (tj3DecompressHeader(tj, jpeg.data(), jpeg.size()) != 0) {
        error_ = std::string("bad JPEG header: ") + tj3GetErrorStr(tj);
        return false;
    }

    const int width = tj3Get(tj, TJPARAM_JPEGWIDTH);
    const int height = tj3Get(tj, TJPARAM_JPEGHEIGHT);
    if (width <= 0 || height <= 0 || width > kMaxDimension || height > kMaxDimension) {
        error_ = "invalid JPEG size " + std::to_string(width) + "x" + std::to_string(height);
        return false;
    }

    out.width = width;
    out.height = height;
    out.pixels.resize(out.stride() * static_cast<size_t>(height));

    // TJPF_BGRA matches the Windows DIB byte order; alpha is filled with 0xFF.
    if (tj3Decompress8(tj, jpeg.data(), jpeg.size(), out.pixels.data(),
                       static_cast<int>(out.stride()), TJPF_BGRA) != 0) {
        // A warning (e.g. a slightly truncated stream) still leaves a usable image,
        // which is better than a frozen viewer. Only fatal errors drop the frame.
        if (tj3GetErrorCode(tj) == TJERR_FATAL) {
            error_ = std::string("decode failed: ") + tj3GetErrorStr(tj);
            return false;
        }
        error_ = std::string("decode warning: ") + tj3GetErrorStr(tj);
    }
    return true;
}

}  // namespace kvm
