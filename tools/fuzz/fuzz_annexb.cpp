// SPDX-License-Identifier: Apache-2.0

// fuzz_annexb: a libFuzzer target for the framing check a viewer runs on an H.264 payload from the
// other PC (kvm/annexb.h, roadmap P2.3c) before a decoder sees it. Built and run like fuzz_protocol:
//   cmake --build --preset x64-fuzz --target fuzz_annexb
//   build\x64-fuzz\tools\fuzz\fuzz_annexb.exe -max_total_time=600 tools\fuzz\corpus_annexb
//
// A crash, an ASan report or one of the checks below failing is a bug.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "kvm/annexb.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> au(data, size);

    // Every NAL unit lies inside the payload, after the one before it.
    std::size_t end = 0;
    for (const auto& nal : kvm::annexb::split(au)) {
        if (nal.offset < end + 3 || nal.offset > size || nal.size > size - nal.offset) std::abort();
        end = nal.offset + nal.size;
    }

    // Access units are back to back, inside the stream.
    std::size_t next = 0;
    for (const auto& [first, second] : kvm::annexb::access_units(au)) {
        if (first < next || second < first || second > size) std::abort();
        next = second;
    }

    // An access unit that passes is a keyframe or not, never both.
    const bool as_keyframe = kvm::annexb::check(au, true).empty();
    const bool as_picture = kvm::annexb::check(au, false).empty();
    if (as_keyframe && as_picture) std::abort();
    return 0;
}
