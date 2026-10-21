// SPDX-License-Identifier: Apache-2.0

// H.264 decoding on the viewer (roadmap P2.4): Annex-B access units in (the "h264" frames of the video
// channel, once kvm/annexb.h's check passed), NV12 pictures out. It is Media Foundation's H.264
// decoder. Given a GpuDevice it decodes on the GPU (DXVA) and pictures stay there as D3D11 textures
// for the renderer; without one, or where the GPU can't, it decodes in software into memory.
//
//   auto dec = kvm::H264Decoder::create(gpu, why);              // gpu may be nullptr: software
//   if (auto picture = dec->decode(frame.payload, frame.header.t_capture_us)) show(*picture);
//
// Every access unit goes in, in order: a picture refers to the ones before it, so the viewer drops
// pictures after decoding, not frames before it. decode() returns the access unit's own picture: the
// decoder runs in low-latency mode (nothing held back for reordering; Sarke's encoder sends no
// B-frames) and is drained after each access unit.
// Not thread-safe: one thread decodes.
#pragma once

#include <d3d11.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "kvm/gpu_device.h"

namespace kvm {

class H264Decoder {
public:
    struct Picture {
        // On the GPU: an NV12 texture of the decoder's (often one slice of a texture array). `sample`
        // keeps it from being re-used until the Picture is gone; hold on to few at a time.
        Microsoft::WRL::ComPtr<IMFSample> sample;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        UINT subresource = 0;
        // In software: NV12 in memory, `stride` bytes per row: `coded_height` rows of Y, then the
        // interleaved UV rows (half as many).
        std::vector<std::uint8_t> nv12;
        int stride = 0;
        int width = 0;   // what to show: the top-left of the picture (the encoder pads 1080 rows to 1088)
        int height = 0;
        int coded_height = 0;
        std::int64_t t_capture_us = 0;  // as given to decode()
    };

    // nullptr with `why` set if Media Foundation has no H.264 decoder here (e.g. Windows N without the
    // Media Feature Pack).
    static std::unique_ptr<H264Decoder> create(std::shared_ptr<GpuDevice> gpu, std::string& why);
    // Whether Media Foundation has an H.264 decoder here (it is only looked up, not started).
    static bool available();
    ~H264Decoder();
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // Decodes one access unit: its picture, waiting up to kPictureWait for the decoder's threads to
    // finish it. nullopt if the decoder refused the data (errors() counts that) or nothing came out.
    std::optional<Picture> decode(std::span<const std::uint8_t> access_unit, std::int64_t t_capture_us);

    // Pictures come out as D3D11 textures (DXVA), not in memory.
    bool on_gpu() const { return on_gpu_; }
    // Access units the decoder refused; pictures that came out but weren't the newest; access units
    // whose picture wasn't out within kPictureWait (it comes with a later one).
    std::uint64_t errors() const { return errors_; }
    std::uint64_t skipped() const { return skipped_; }
    std::uint64_t late() const { return late_; }

    static constexpr std::chrono::milliseconds kPictureWait{100};
    const std::string& name() const { return name_; }

private:
    H264Decoder() = default;
    bool set_output_type();
    bool take_pictures(std::optional<Picture>& newest);
    std::optional<Picture> picture_from(Microsoft::WRL::ComPtr<IMFSample> sample);

    std::shared_ptr<GpuDevice> gpu_;
    std::string name_;
    Microsoft::WRL::ComPtr<IMFActivate> activate_;
    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager_;
    bool started_mf_ = false;
    bool on_gpu_ = false;
    bool provides_samples_ = false;
    std::uint32_t output_size_ = 0;
    int coded_width_ = 0;
    int coded_height_ = 0;
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    std::uint64_t errors_ = 0;
    std::uint64_t skipped_ = 0;
    std::uint64_t late_ = 0;
};

}  // namespace kvm
