// SPDX-License-Identifier: Apache-2.0

// h264_test: H.264 on the GPU, both ways (host_a/h264_encoder.h, client_b/h264_decoder.h, roadmap
// P2.3/P2.4). Needs a hardware H.264 encoder (ctest label "gpu": local only). 60 moving 1920x1080
// pictures at 30 fps go BGRA -> NV12 -> encoder, and it checks:
//   - every picture comes out as one Annex-B access unit, unless the encoder was still busy (a few may be
//     dropped then: the newest picture counts);
//   - each passes the viewer's framing check (kvm/annexb.h): the first is a keyframe with SPS, PPS and
//     an IDR picture, and so is every other keyframe;
//   - a keyframe follows request_keyframe(), and otherwise there are none (a long GOP);
//   - capture -> encoded time and the bitrate against the target (printed);
//   - on the video channel (TLS over 127.0.0.1), every access unit arrives as a "h264" frame with
//     its keyframe flag, byte for byte;
//   - decoded on the GPU again (a device of its own, like the viewer's), each decode() returns that
//     access unit's picture, 1920x1080, and its luma matches the source above a PSNR floor (printed,
//     with the time of a decode() call).
//
//   h264_test.exe                          (exit code 0 = all passed)
//   h264_test.exe --write-sample <file>    writes tools/data/h264_sample.h264 for h264_decode_test

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string_view>
#include <thread>
#include <vector>

#include "h264_decoder.h"
#include "h264_encoder.h"
#include "kvm/annexb.h"
#include "kvm/gpu_device.h"
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
constexpr double kMinPsnr = 30.0;  // dB, luma; H.264 at 8 Mbit/s on this content is well above it

using Draw = std::function<void(std::vector<std::uint8_t>& bgra, int frame)>;

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

// The sample for h264_decode_test (320x240): red, green, blue and gray quadrants, and a white block
// moving along the middle so the pictures after the first are not empty.
constexpr int kSampleWidth = 320, kSampleHeight = 240, kSampleFrames = 10;
void draw_sample(std::vector<std::uint8_t>& px, int frame) {
    for (int y = 0; y < kSampleHeight; ++y) {
        for (int x = 0; x < kSampleWidth; ++x) {
            std::uint8_t* p = &px[(static_cast<std::size_t>(y) * kSampleWidth + x) * 4];
            const bool right = x >= kSampleWidth / 2, bottom = y >= kSampleHeight / 2;
            const bool block = std::abs(x - (40 + frame * 24)) < 8 && std::abs(y - kSampleHeight / 2) < 8;
            // BGRA: red, green / blue, gray.
            static constexpr std::uint8_t kColours[4][3] = {{0, 0, 255}, {0, 255, 0}, {255, 0, 0}, {128, 128, 128}};
            const auto& c = kColours[(bottom ? 2 : 0) + (right ? 1 : 0)];
            p[0] = block ? 255 : c[0];
            p[1] = block ? 255 : c[1];
            p[2] = block ? 255 : c[2];
            p[3] = 255;
        }
    }
}

struct Encoded {
    std::vector<kvm::H264Encoder::Packet> packets;
    std::vector<std::int64_t> frame_times;  // the capture time given for each picture
    std::int64_t keyframe_asked_at = 0;
    double seconds = 0;
    bool flushed = false;
};

// Draws, converts and encodes `frames` pictures at kFps, asking for a keyframe before `keyframe_at`.
Encoded encode(kvm::GpuDevice& gpu, kvm::H264Encoder& encoder, kvm::Nv12Converter& nv12, int width, int height,
               int frames, int keyframe_at, const Draw& draw_frame) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(width);
    d.Height = static_cast<UINT>(height);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> source;
    gpu.device()->CreateTexture2D(&d, nullptr, &source);

    Encoded e;
    const auto collect = [&] {
        while (auto p = encoder.packets().try_pop()) e.packets.push_back(std::move(*p));
    };
    std::vector<std::uint8_t> px(static_cast<std::size_t>(width) * height * 4);
    auto next = std::chrono::steady_clock::now();
    const std::int64_t started = kvm::steady_now_us();
    for (int i = 0; i < frames; ++i) {
        draw_frame(px, i);
        {
            const kvm::GpuDevice::Lock lock(gpu);
            gpu.context()->UpdateSubresource(source.Get(), 0, nullptr, px.data(), static_cast<UINT>(width) * 4, 0);
        }
        const std::int64_t t = kvm::steady_now_us();
        if (i == keyframe_at) {
            encoder.request_keyframe();
            e.keyframe_asked_at = t;
        }
        ID3D11Texture2D* picture = nv12.convert(source.Get(), width, height);
        if (!picture || !encoder.encode(picture, t)) break;
        e.frame_times.push_back(t);
        collect();
        next += std::chrono::microseconds(1'000'000 / kFps);
        std::this_thread::sleep_until(next);
    }
    e.flushed = encoder.flush(3s);
    collect();
    e.seconds = static_cast<double>(kvm::steady_now_us() - started) / 1e6;
    return e;
}

// The luma the converter makes of a BGRA pixel: BT.709, limited range (nv12_converter.h).
double luma(const std::uint8_t* bgra) {
    return 16.0 + 219.0 * (0.2126 * bgra[2] + 0.7152 * bgra[1] + 0.0722 * bgra[0]) / 255.0;
}

// The luma PSNR of a decoded picture's Y plane (rows of `pitch` bytes) against source picture `frame`.
double psnr_y(const std::uint8_t* y_plane, std::size_t pitch, int frame) {
    static std::vector<std::uint8_t> px(static_cast<std::size_t>(kWidth) * kHeight * 4);
    draw(px, frame);
    double squared = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const double diff = y_plane[static_cast<std::size_t>(y) * pitch + x] -
                                luma(&px[(static_cast<std::size_t>(y) * kWidth + x) * 4]);
            squared += diff * diff;
        }
    }
    const double mse = squared / (static_cast<double>(kWidth) * kHeight);
    return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// A decoded picture's Y plane, read back from its texture: `pitch` bytes per row, picture.height rows.
std::vector<std::uint8_t> read_luma(kvm::GpuDevice& gpu, const kvm::H264Decoder::Picture& picture, std::size_t& pitch) {
    std::vector<std::uint8_t> y;
    D3D11_TEXTURE2D_DESC d{};
    picture.texture->GetDesc(&d);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(gpu.device()->CreateTexture2D(&d, nullptr, &staging))) return y;
    const kvm::GpuDevice::Lock lock(gpu);
    gpu.context()->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, picture.texture.Get(), picture.subresource,
                                         nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(gpu.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return y;
    pitch = mapped.RowPitch;
    const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData);
    y.assign(bytes, bytes + pitch * static_cast<std::size_t>(picture.height));
    gpu.context()->Unmap(staging.Get(), 0);
    return y;
}

int write_sample(const char* path) {
    auto gpu = kvm::GpuDevice::create();
    std::string why;
    auto encoder = gpu ? kvm::H264Encoder::create(gpu,
                                                  {.width = kSampleWidth,
                                                   .height = kSampleHeight,
                                                   .fps = kFps,
                                                   .bitrate = 1'000'000,
                                                   .keyframe_interval_s = 60},
                                                  why)
                       : nullptr;
    if (!encoder) {
        kvm::log::error("no hardware encoder: {}", why);
        return 1;
    }
    kvm::Nv12Converter nv12(gpu);
    const auto e = encode(*gpu, *encoder, nv12, kSampleWidth, kSampleHeight, kSampleFrames, -1, draw_sample);
    if (!e.flushed || e.packets.size() != kSampleFrames) {
        kvm::log::error("{} access units for {} pictures", e.packets.size(), kSampleFrames);
        return 1;
    }
    std::ofstream file(path, std::ios::binary);
    for (const auto& p : e.packets) {
        file.write(reinterpret_cast<const char*>(p.annexb.data()), static_cast<std::streamsize>(p.annexb.size()));
    }
    kvm::log::info("{}: {} access units, {} bytes", path, e.packets.size(), static_cast<long long>(file.tellp()));
    return file ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--write-sample") return write_sample(argv[2]);

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

    const auto e = encode(*gpu, *encoder, nv12, kWidth, kHeight, kFrames, kKeyframeAt, draw);
    const auto& out = e.packets;
    check(e.flushed, "every picture came out");

    // When the encoder is still busy and two pictures wait, the older is dropped (the newest counts).
    const auto dropped = encoder->dropped();
    check(e.frame_times.size() == kFrames && out.size() + dropped == e.frame_times.size() && dropped <= 3,
          std::format("{} access units for {} of {} pictures, {} dropped while the encoder was busy, {} skipped by it",
                      out.size(), e.frame_times.size(), kFrames, dropped, encoder->skipped()));
    if (out.empty()) return 1;

    const auto first = nal_types(out.front().annexb);
    check(out.front().keyframe && first.contains(7) && first.contains(8) && first.contains(5),
          "the first is a keyframe with SPS, PPS and an IDR picture");
    std::vector<std::size_t> idr;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (nal_types(out[i].annexb).contains(5)) idr.push_back(i);
    }
    // The picture given to encode() right after the request (or, if it was dropped, the next one).
    const auto asked =
        std::find_if(out.begin(), out.end(), [&](const auto& o) { return o.t_capture_us >= e.keyframe_asked_at; });
    const auto asked_at = static_cast<std::size_t>(asked - out.begin());
    check(asked != out.end() && std::find(idr.begin(), idr.end(), asked_at) != idr.end(),
          std::format("request_keyframe(): the next picture (access unit {}) is an IDR", asked_at));
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
    check(
        framed == out.size(),
        std::format("{} of {} pass the viewer's framing check (SPS and PPS with every keyframe)", framed, out.size()));

    std::vector<double> latency;
    std::size_t bytes = 0;
    for (const auto& o : out) {
        latency.push_back(static_cast<double>(o.t_encoded_us - o.t_capture_us) / 1000.0);
        bytes += o.annexb.size();
    }
    std::sort(latency.begin(), latency.end());
    const double mbps = static_cast<double>(bytes) * 8.0 / e.seconds / 1e6;
    kvm::log::info("       picture -> access unit (NV12, copy, encode): p50 {:.1f} ms, p95 {:.1f} ms",
                   latency[latency.size() / 2], latency[latency.size() * 95 / 100]);
    check(mbps > 0.25 * kBitrate / 1e6 && mbps < 2.0 * kBitrate / 1e6,
          std::format("{:.2f} Mbit/s for a {:.1f} Mbit/s target", mbps, kBitrate / 1e6));

    const auto received = over_the_wire(*encoder, out);
    bool intact = received.size() == out.size();
    for (std::size_t i = 0; intact && i < received.size(); ++i) {
        const auto& h = received[i].header;
        intact = h.codec == "h264" && h.seq == i && h.keyframe == out[i].keyframe && h.width == kWidth &&
                 h.height == kHeight && h.t_capture_us == out[i].t_capture_us && received[i].payload == out[i].annexb;
    }
    check(intact, std::format("video channel: {} of {} arrived as \"h264\" frames, keyframe flag and bytes intact",
                              received.size(), out.size()));

    // The viewer's side: its own device, every access unit in order.
    auto viewer_gpu = kvm::GpuDevice::create();
    auto decoder = viewer_gpu ? kvm::H264Decoder::create(viewer_gpu, why) : nullptr;
    check(decoder != nullptr, decoder ? std::format("a decoder: {}", decoder->name()) : why);
    if (!decoder) return 1;
    std::map<std::int64_t, int> frame_of;
    for (std::size_t i = 0; i < e.frame_times.size(); ++i) frame_of[e.frame_times[i]] = static_cast<int>(i);
    std::vector<double> decode_ms, psnr;
    double wrong_source = 99.0;  // the first picture against another source picture: must be far lower
    std::size_t at_once = 0, right_size = 0;
    for (const auto& o : out) {
        const std::int64_t t0 = kvm::steady_now_us();
        auto picture = decoder->decode(o.annexb, o.t_capture_us);
        decode_ms.push_back(static_cast<double>(kvm::steady_now_us() - t0) / 1000.0);
        if (!picture) continue;
        if (picture->t_capture_us == o.t_capture_us) ++at_once;
        if (picture->width == kWidth && picture->height == kHeight && picture->texture) ++right_size;
        const auto frame = frame_of.find(picture->t_capture_us);
        std::size_t pitch = 0;
        const auto y = picture->texture ? read_luma(*viewer_gpu, *picture, pitch) : std::vector<std::uint8_t>{};
        if (frame != frame_of.end() && !y.empty()) {
            psnr.push_back(psnr_y(y.data(), pitch, frame->second));
            if (psnr.size() == 1) wrong_source = psnr_y(y.data(), pitch, frame->second + 10);
        }
    }
    check(decoder->on_gpu(), "decoded on the GPU (DXVA): pictures are D3D11 textures");
    check(at_once == out.size() && decoder->errors() == 0,
          std::format("{} of {} decode() calls returned their own picture (low latency), {} refused", at_once,
                      out.size(), decoder->errors()));
    check(right_size == out.size(),
          std::format("{} of {} pictures are {}x{}", right_size, out.size(), kWidth, kHeight));
    std::sort(decode_ms.begin(), decode_ms.end());
    kvm::log::info("       decode() call: p50 {:.1f} ms, p95 {:.1f} ms", decode_ms[decode_ms.size() / 2],
                   decode_ms[decode_ms.size() * 95 / 100]);
    if (!psnr.empty()) {
        const double low = *std::min_element(psnr.begin(), psnr.end());
        double mean = 0;
        for (const double v : psnr) mean += v / static_cast<double>(psnr.size());
        check(psnr.size() == out.size() && low >= kMinPsnr,
              std::format("luma PSNR against the source: {} pictures, mean {:.1f} dB, lowest {:.1f} dB (floor {:.0f})",
                          psnr.size(), mean, low, kMinPsnr));
        check(wrong_source < kMinPsnr,
              std::format("the comparison tells pictures apart: against the source 10 frames later, {:.1f} dB",
                          wrong_source));
    } else {
        check(false, "luma PSNR: no picture could be read back");
    }

    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
