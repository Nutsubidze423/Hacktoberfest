#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace kvm {

// Decoded frame: top-down rows, 4 bytes per pixel in B, G, R, A order, no row padding.
// Draw it with StretchDIBits using a BITMAPINFO with biHeight = -height (top-down).
struct BgraImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;  // width * height * 4 bytes

    size_t stride() const { return static_cast<size_t>(width) * 4; }
    bool empty() const { return pixels.empty(); }
};

// Thin RAII wrapper around a TurboJPEG decompressor.
// Not thread-safe: use one instance per thread (e.g. one in the video receive thread).
class JpegDecoder {
public:
    JpegDecoder();

    // Decodes `jpeg` into `out`. Reuses out.pixels' memory, so passing the same
    // BgraImage every frame avoids a per-frame allocation.
    // Returns false and sets last_error() if the data is not a usable JPEG.
    bool decode(std::span<const uint8_t> jpeg, BgraImage& out);

    const std::string& last_error() const { return error_; }

private:
    struct HandleDeleter {
        void operator()(void* handle) const;
    };

    std::unique_ptr<void, HandleDeleter> handle_;  // tjhandle
    std::string error_;
};

}  // namespace kvm
