// TurboJPEG encoder for captured frames: BGRX input, 4:2:0 chroma subsampling.
// Not thread-safe; use one instance per thread (the video sender thread).
//
//   kvm::JpegEncoder jpeg(config.jpeg_quality);
//   std::vector<std::uint8_t> out;
//   if (jpeg.encode(frame, out)) channel.send_frame(header, out);   // jpeg.last_encode_ms() for logs
#pragma once

#include <cstdint>
#include <vector>

#include "capture.h"

namespace kvm {

class JpegEncoder {
public:
    // quality 1..100 (config.json jpeg_quality). Throws std::runtime_error if TurboJPEG can't start.
    explicit JpegEncoder(int quality = 75);
    ~JpegEncoder();
    JpegEncoder(const JpegEncoder&) = delete;
    JpegEncoder& operator=(const JpegEncoder&) = delete;

    // Replaces `out` with the JPEG of `frame`. False on error (logged), `out` is then empty.
    bool encode(const CapturedFrame& frame, std::vector<std::uint8_t>& out);

    int quality() const { return quality_; }

    // Duration of the last successful encode() in milliseconds.
    double last_encode_ms() const { return last_ms_; }

private:
    void* handle_ = nullptr;  // tjhandle; keeps turbojpeg.h out of this header
    int quality_;
    double last_ms_ = 0.0;
};

}  // namespace kvm
