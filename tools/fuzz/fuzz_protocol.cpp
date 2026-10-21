// SPDX-License-Identifier: Apache-2.0

// fuzz_protocol: a libFuzzer target for what Sarke parses from another PC (roadmap P1.12):
//   - a control message, proto::decode: also the pairing messages a key that isn't paired may send;
//   - a video frame header, FrameHeader, read the way FrameChannel::recv_frame reads it.
// Only header-only code is compiled in, so ASan instruments all of it.
//
// Built only in the x64-fuzz preset (SARKE_FUZZ=ON, MSVC /fsanitize=fuzzer,address), from a
// Developer PowerShell (it puts the ASan runtime DLL on PATH):
//   cmake --preset x64-fuzz
//   cmake --build --preset x64-fuzz --target fuzz_protocol
//   build\x64-fuzz\tools\fuzz\fuzz_protocol.exe -max_total_time=600 tools\fuzz\corpus
//
// A crash, an ASan report or the round-trip check below failing is a bug. libFuzzer writes the
// input as crash-<hash> in the current folder.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include <nlohmann/json.hpp>

#include "kvm/protocol.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const auto j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return 0;

    // The control channel: never throws; anything unknown or malformed is nullopt.
    if (const auto msg = kvm::proto::decode(j)) {
        // What was accepted must survive being sent on: encode, then decode, gives the same type.
        const auto again = kvm::proto::decode(kvm::proto::encode(*msg));
        if (!again || again->index() != msg->index()) std::abort();
    }

    // The video channel's header (FrameChannel::recv_frame catches the same exceptions).
    try {
        (void)j.get<kvm::proto::FrameHeader>();
    } catch (const nlohmann::json::exception&) {}
    return 0;
}
