// SPDX-License-Identifier: Apache-2.0

// h264_test: H.264 on the GPU (host_a/h264_encoder.h, roadmap P2.3b/c). Needs a hardware H.264 encoder
// (ctest label "gpu": local only). 60 moving 1920x1080 pictures at 30 fps go BGRA -> NV12 -> encoder,
// and it checks:
//   - every picture comes out as one Annex-B access unit, unless the encoder was still busy (a few may be
//     dropped then: the newest picture counts);
//   - each passes the viewer's framing check (kvm/annexb.h): the first is a keyframe with SPS, PPS and
//     an IDR picture, and so is every other keyframe;
//   - a keyframe follows request_keyframe(), and otherwise there are none (a long GOP);
//   - capture -> encoded time and the bitrate against the target (printed);
//   - on the video channel (TLS over 127.0.0.1), every access unit arrives as a "h264" frame with
//     its keyframe flag, byte for byte.
// Decoding it back and comparing (PSNR) comes with the decoder, P2.4.
//
//   h264_test.exe   (exit code 0 = all passed)

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <numeric>
#include <set>
#include <string_view>
#include <thread>
#include <vector>

#include "gpu_device.h"
#include "h264_encoder.h"
#include "kvm/annexb.h"
#include "kvm/log.h"
#include "kvm/net.h"
#include "kvm/video.h"
#include "nv12_converter.h"
#include "side_channel_util.h"

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

namespace {

struct Checks {
    int failed = 0;
    void operator()(bool ok, std::string_view what) {
        kvm::log::info("{} {}", ok ? "[ok]  " : "[FAIL]", what);
        if (!ok) ++failed;
    }
};

// The NAL unit types in an Annex-B access unit (5 = IDR picture, 7 = SPS, 8 = PPS, 1 = other picture).
std::set<int> nal_types(const std::vector<std::uint8_t>& au) {
    std::set<int> types;
    for (const auto& nal : kvm::annexb::split(au)) types.insert(nal.type);
    return types;
}

// Sends `packets` over a video channel on 127.0.0.1 the way kvm_host will, and returns what the viewer
// side received.
std::vector<kvm::net::Frame> over_the_wire(const kvm::H264Encoder& encoder,
                                           const std::vector<kvm::H264Encoder::Packet>& packets) {
    std::uint16_t port = 0;
    const SOCKET listener = kvm::test::listen_loopback(port);
    const kvm::test::Session session;
    std::unique_ptr<kvm::net::FrameChannel> host;
    std::thread acceptor([&] {
        if (auto b = kvm::test::admit_socket(accept(listener, nullptr, nullptr), session, "video", "viewer")) {
            host = std::make_unique<kvm::net::FrameChannel>(std::move(*b));
        }
    });
    auto viewer = kvm::net::connect_video("127.0.0.1", port, session.client, session.host.id(), session.token);
    acceptor.join();
    closesocket(listener);
    std::vector<kvm::net::Frame> received;
    if (!host || !viewer) return received;
    std::thread sender([&] {
        std::uint32_t seq = 0;
        for (const auto& packet : packets) host->send_frame(encoder.header_for(packet, seq++), packet.annexb);
    });
    while (received.size() < packets.size()) {
        auto frame = viewer->recv_frame();
        if (!frame) break;
        received.push_back(std::move(*frame));
    }
    sender.join();
    host->close();
    viewer->close();
    return received;
}

constexpr int kWidth = 1920, kHeight = 1080, kFps = 30, kFrames = 60, kBitrate = 8'000'000, kKeyframeAt = 30;

// A picture that changes every frame, like a video: a gradient and a moving block.
void draw(std::vector<std::uint8_t>& px, int frame) {
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            std::uint8_t* p = &px[(static_cast<std::size_t>(y) * kWidth + x) * 4];
            const bool block = std::abs(x - (frame * 17) % kWidth) < 80 && std::abs(y - kHeight / 2) < 80;
            p[0] = block ? 255 : static_cast<std::uint8_t>(x / 5);
            p[1] = block ? 80 : static_cast<std::uint8_t>(y / 3);
            p[2] = block ? 30 : static_cast<std::uint8_t>((x + y + frame * 4) / 8);
            p[3] = 255;
        }
    }
}

}  // namespace

int main() {
    Checks check;
    auto gpu = kvm::GpuDevice::create();
    check(gpu != nullptr, "a hardware D3D11 device with video support");
    if (!gpu) return 1;
    std::string why;
    auto encoder = kvm::H264Encoder::create(
        gpu, {.width = kWidth, .height = kHeight, .fps = kFps, .bitrate = kBitrate, .keyframe_interval_s = 60}, why);
    check(encoder != nullptr, encoder ? std::format("a hardware encoder: {}", encoder->name()) : why);
    if (!encoder) return 1;
    kvm::Nv12Converter nv12(gpu);

    D3D11_TEXTURE2D_DESC d{};
    d.Width = kWidth;
    d.Height = kHeight;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> source;
    gpu->device()->CreateTexture2D(&d, nullptr, &source);

    std::vector<kvm::H264Encoder::Packet> out;
    const auto collect = [&] {
        while (auto p = encoder->packets().try_pop()) out.push_back(std::move(*p));
    };

    std::vector<std::uint8_t> px(static_cast<std::size_t>(kWidth) * kHeight * 4);
    std::int64_t keyframe_asked_at = 0;
    auto next = std::chrono::steady_clock::now();
    const std::int64_t started = kvm::steady_now_us();
    for (int i = 0; i < kFrames; ++i) {
        draw(px, i);
        {
            const kvm::GpuDevice::Lock lock(*gpu);
            gpu->context()->UpdateSubresource(source.Get(), 0, nullptr, px.data(), kWidth * 4, 0);
        }
        const std::int64_t t = kvm::steady_now_us();
        if (i == kKeyframeAt) {
            encoder->request_keyframe();
            keyframe_asked_at = t;
        }
        ID3D11Texture2D* picture = nv12.convert(source.Get(), kWidth, kHeight);
        if (!picture || !encoder->encode(picture, t)) break;
        collect();
        next += std::chrono::microseconds(1'000'000 / kFps);
        std::this_thread::sleep_until(next);
    }
    check(encoder->flush(3s), "every picture came out");
    collect();
    const double seconds = static_cast<double>(kvm::steady_now_us() - started) / 1e6;

    // When the encoder is still busy and two pictures wait, the older is dropped (the newest counts).
    const auto dropped = encoder->dropped();
    check(out.size() + dropped == static_cast<std::size_t>(kFrames) && dropped <= 3,
          std::format("{} access units for {} pictures, {} dropped while the encoder was busy", out.size(), kFrames,
                      dropped));
    if (!out.empty()) {
        const auto first = nal_types(out.front().annexb);
        check(out.front().keyframe && first.contains(7) && first.contains(8) && first.contains(5),
              "the first is a keyframe with SPS, PPS and an IDR picture");
        std::vector<std::size_t> idr;
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (nal_types(out[i].annexb).contains(5)) idr.push_back(i);
        }
        const bool requested = std::any_of(idr.begin(), idr.end(), [&](std::size_t i) {
            return out[i].t_capture_us >= keyframe_asked_at && i <= kKeyframeAt + 3;
        });
        check(requested, "request_keyframe(): the next picture is an IDR");
        check(idr.size() == 2, std::format("no other keyframes (a long GOP): {} IDR pictures in all", idr.size()));
        std::size_t framed = 0;
        for (std::size_t i = 0; i < out.size(); ++i) {
            const auto refused = kvm::annexb::check(out[i].annexb, out[i].keyframe);
            if (refused.empty()) {
                ++framed;
            } else {
                kvm::log::info("       access unit {} refused: {}", i, refused);
            }
        }
        check(framed == out.size(),
              std::format("{} of {} pass the viewer's framing check (SPS and PPS with every keyframe)", framed,
                          out.size()));

        std::vector<double> latency;
        std::size_t bytes = 0;
        for (const auto& o : out) {
            latency.push_back(static_cast<double>(o.t_encoded_us - o.t_capture_us) / 1000.0);
            bytes += o.annexb.size();
        }
        std::sort(latency.begin(), latency.end());
        const double mbps = static_cast<double>(bytes) * 8.0 / seconds / 1e6;
        kvm::log::info("       picture -> access unit (NV12, copy, encode): p50 {:.1f} ms, p95 {:.1f} ms",
                       latency[latency.size() / 2], latency[latency.size() * 95 / 100]);
        check(mbps > 0.25 * kBitrate / 1e6 && mbps < 2.0 * kBitrate / 1e6,
              std::format("{:.2f} Mbit/s for a {:.1f} Mbit/s target", mbps, kBitrate / 1e6));

        const auto received = over_the_wire(*encoder, out);
        bool intact = received.size() == out.size();
        for (std::size_t i = 0; intact && i < received.size(); ++i) {
            const auto& h = received[i].header;
            intact = h.codec == "h264" && h.seq == i && h.keyframe == out[i].keyframe && h.width == kWidth &&
                     h.height == kHeight && h.t_capture_us == out[i].t_capture_us &&
                     received[i].payload == out[i].annexb;
        }
        check(intact, std::format("video channel: {} of {} arrived as \"h264\" frames, keyframe flag and bytes intact",
                                  received.size(), out.size()));
    }

    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
