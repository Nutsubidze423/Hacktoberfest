// SPDX-License-Identifier: Apache-2.0

// What this PC can decode and encode for window streams (roadmap P2.5), for its hello
// (proto::Hello::video_decode / video_encode; the rule is kvm/codec.h):
//   - "jpeg" both ways, always;
//   - "h264" to decode when Media Foundation has an H.264 decoder (Windows N without the Media
//     Feature Pack doesn't), to encode when the GPU has a hardware H.264 encoder.
//
//   const auto codecs = kvm::probe_video_codecs();   // once at start: it makes a D3D11 device (~100 ms)
#pragma once

#include <string>
#include <vector>

namespace kvm {

struct VideoCodecs {
    std::vector<std::string> decode;
    std::vector<std::string> encode;
};

VideoCodecs probe_video_codecs();

}  // namespace kvm
