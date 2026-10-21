// SPDX-License-Identifier: Apache-2.0

// Checks kvm/annexb.h, the framing check a viewer runs on an H.264 payload from the other PC:
// NAL units split on 3- and 4-byte start codes, and what check() accepts (an encoder's access
// units) and refuses (no start code, empty or forbidden NAL units, no picture, a keyframe flag that
// doesn't match, an IDR picture without SPS and PPS).
// Usage: annexb_test.exe   (exit code 0 = all passed)

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "kvm/annexb.h"

namespace annexb = kvm::annexb;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

using Bytes = std::vector<std::uint8_t>;

// An access unit of NAL units, each after a 4-byte start code (as Media Foundation writes them),
// or a 3-byte one with `short_codes`.
Bytes au(std::initializer_list<Bytes> nals, bool short_codes = false) {
    Bytes out;
    for (const auto& nal : nals) {
        if (!short_codes) out.push_back(0);
        out.insert(out.end(), {0, 0, 1});
        out.insert(out.end(), nal.begin(), nal.end());
    }
    return out;
}

const Bytes kAud = {0x09, 0xF0};
const Bytes kSps = {0x67, 0x4D, 0x40, 0x28, 0x95};
const Bytes kPps = {0x68, 0xEE, 0x3C, 0x80};
const Bytes kSei = {0x06, 0x05, 0x10, 0x80};
const Bytes kIdr = {0x65, 0x88, 0x84, 0x00, 0x21};  // a zero byte inside is fine
const Bytes kP = {0x41, 0x9A, 0x02};

bool refused(const Bytes& bytes, bool keyframe, std::string_view why) {
    const auto got = annexb::check(bytes, keyframe);
    if (got != why) std::printf("       got \"%.*s\"\n", static_cast<int>(got.size()), got.data());
    return got == why;
}

void test_split() {
    const Bytes mixed = {0, 0, 0, 1, 0x67, 0x42, 0, 0, 1, 0x68, 0xCE, 0, 0, 0, 1, 0x65, 0x88, 0, 0};
    const auto nals = annexb::split(mixed);
    check(nals.size() == 3 && nals[0].type == 7 && nals[0].offset == 4 && nals[0].size == 2 && nals[1].type == 8 &&
              nals[1].offset == 9 && nals[1].size == 2 && nals[2].type == 5 && nals[2].offset == 15 &&
              nals[2].size == 2,
          "split: 4- and 3-byte start codes; zeros before a start code and at the end are not part of a NAL unit");
    check(annexb::split(Bytes{}).empty() && annexb::split(Bytes{0, 0}).empty() &&
              annexb::split(Bytes{0x65, 0x88}).empty(),
          "split: nothing without a start code");
    const auto tail = annexb::split(Bytes{0, 0, 1});
    check(tail.size() == 1 && tail[0].size == 0, "split: a start code at the very end is an empty NAL unit");
}

void test_accepted() {
    check(annexb::check(au({kAud, kSps, kPps, kSei, kIdr}), true).empty(),
          "a keyframe: AUD, SPS, PPS, SEI, IDR picture");
    check(annexb::check(au({kSps, kPps, kIdr, kIdr}), true).empty(), "a keyframe in two slices");
    check(annexb::check(au({kAud, kP}), false).empty(), "a picture that refers to earlier ones");
    check(annexb::check(au({kP, kP, kP}, true), false).empty(), "3-byte start codes, three slices");
    Bytes padded = {0, 0};
    const Bytes p = au({kP});
    padded.insert(padded.end(), p.begin(), p.end());
    padded.insert(padded.end(), {0, 0, 0});
    check(annexb::check(padded, false).empty(), "zero bytes before the first start code and after the last NAL unit");
}

void test_refused() {
    check(refused(Bytes{}, false, "no start code"), "empty payload");
    check(refused(Bytes{0x00, 0x00, 0x00, 0x1F, 0x41, 0x9A}, false, "no start code"),
          "length-prefixed (AVCC), not Annex-B");
    Bytes junk = {0x12};
    const Bytes p = au({kP});
    junk.insert(junk.end(), p.begin(), p.end());
    check(refused(junk, false, "bytes before the first start code"), "a non-zero byte before the first start code");
    check(refused(au({kSps, kPps, Bytes{0, 0}, kIdr}), true, "an empty NAL unit"), "an empty NAL unit");
    check(refused(au({Bytes{0xC1, 0x9A}}), false, "a NAL unit with the forbidden bit set"), "the forbidden bit");
    check(refused(au({Bytes{0x7C, 0x85}}), false, "a NAL unit type that is not for a stream"),
          "type 28 (an RTP fragment, not Annex-B)");
    check(refused(au({kAud, kSps, kPps}), true, "no picture"), "SPS and PPS but no picture");
    check(refused(au({kSps, kPps, kIdr, kP}), true, "IDR and non-IDR slices in one picture"),
          "IDR and non-IDR slices together");
    check(refused(au({kAud, kIdr}), true, "an IDR picture without SPS and PPS before it"),
          "an IDR picture without SPS and PPS: a viewer couldn't start there");
    check(refused(au({kSps, kIdr, kPps}), true, "an IDR picture without SPS and PPS before it"),
          "PPS after the IDR picture");
    check(refused(au({kP}), true, "marked as a keyframe but not an IDR picture"), "keyframe flag on a P picture");
    check(refused(au({kSps, kPps, kIdr}), false, "an unmarked IDR picture"), "an IDR picture without the flag");
}

}  // namespace

int main() {
    test_split();
    test_accepted();
    test_refused();
    std::printf("\n%s\n", g_failures == 0 ? "all passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
