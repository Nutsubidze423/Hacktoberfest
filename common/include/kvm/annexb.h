// SPDX-License-Identifier: Apache-2.0

// H.264 access units in Annex-B form, the payload of a `frame` with codec "h264" (docs/PROTOCOL.md):
// NAL units, each after a start code 00 00 01 (or 00 00 00 01).
//
//   for (const auto& nal : kvm::annexb::split(payload)) { ... nal.type ... }
//   if (const auto why = kvm::annexb::check(payload, header.keyframe); !why.empty()) { drop the frame }
//
// The payload comes from the other PC: the viewer runs check() before a decoder sees it, and
// tools/fuzz/fuzz_annexb.cpp fuzzes both. It checks the framing only; what is inside the NAL
// units is the decoder's to check. Header-only, so the fuzz target instruments all of it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace kvm::annexb {

// The nal_unit_type values Sarke looks at.
inline constexpr int kSlice = 1;     // a picture (or part of one) that refers to earlier pictures
inline constexpr int kIdrSlice = 5;  // a picture that refers to none: decoding can start here
inline constexpr int kSps = 7;       // sequence parameter set
inline constexpr int kPps = 8;       // picture parameter set

struct Nal {
    int type = 0;            // nal_unit_type, the low 5 bits of the NAL unit's first byte
    std::size_t offset = 0;  // where the NAL unit starts (its header byte), just after the start code
    std::size_t size = 0;    // up to the next start code, without the zero bytes in front of it
};

// The NAL units of `au`, in order. Bytes before the first start code belong to none.
inline std::vector<Nal> split(std::span<const std::uint8_t> au) {
    std::vector<Nal> nals;
    const auto close = [&](std::size_t start, std::size_t end) {
        // A NAL unit never ends in a zero byte: zeros here belong to the next start code.
        while (end > start && au[end - 1] == 0) --end;
        nals.push_back({.type = end > start ? au[start] & 0x1F : 0, .offset = start, .size = end - start});
    };
    bool open = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i + 2 < au.size();) {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            if (open) close(start, i);
            open = true;
            start = i + 3;
            i += 3;
        } else {
            ++i;
        }
    }
    if (open) close(start, au.size());
    return nals;
}

// Why `au` is not one well-formed access unit whose frame header says `keyframe`, or "" if it is:
//   - it starts with a start code (zero bytes may come before it), and no NAL unit is empty or has
//     the forbidden bit set;
//   - every nal_unit_type is one H.264 defines for a stream (1-23; 0 and 24-31 are not);
//   - it holds one picture: slices that are either all IDR or all not;
//   - keyframe is true exactly when it is an IDR picture, and then SPS and PPS come before the
//     picture, so a viewer can start decoding there.
inline std::string_view check(std::span<const std::uint8_t> au, bool keyframe) {
    const auto nals = split(au);
    if (nals.empty()) return "no start code";
    for (std::size_t i = 0; i + 3 < nals.front().offset; ++i) {
        if (au[i] != 0) return "bytes before the first start code";
    }
    bool sps = false, pps = false, idr = false, slice = false;
    for (const Nal& nal : nals) {
        if (nal.size == 0) return "an empty NAL unit";
        if ((au[nal.offset] & 0x80) != 0) return "a NAL unit with the forbidden bit set";
        if (nal.type == 0 || nal.type > 23) return "a NAL unit type that is not for a stream";
        if (nal.type == kSps) sps = true;
        if (nal.type == kPps) pps = true;
        if (nal.type == kSlice) slice = true;
        if (nal.type == kIdrSlice) {
            if (!sps || !pps) return "an IDR picture without SPS and PPS before it";
            idr = true;
        }
    }
    if (!idr && !slice) return "no picture";
    if (idr && slice) return "IDR and non-IDR slices in one picture";
    if (idr != keyframe) return keyframe ? "marked as a keyframe but not an IDR picture" : "an unmarked IDR picture";
    return {};
}

}  // namespace kvm::annexb
