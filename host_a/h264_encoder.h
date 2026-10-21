// SPDX-License-Identifier: Apache-2.0

// H.264 on the GPU with a Media Foundation hardware encoder (roadmap P2.3b): NV12 textures in (from
// Nv12Converter, on the same GpuDevice), Annex-B access units out. Frames never leave the GPU: the
// encoder gets the device through IMFDXGIDeviceManager.
//
//   auto enc = kvm::H264Encoder::create(gpu, {.width = 1920, .height = 1080, .fps = 30, .bitrate = 8'000'000});
//   enc->encode(nv12.convert(bgra, w, h), t_capture_us);   // returns at once
//   while (auto packet = enc->packets().pop_for(...))
//       channel.send_frame(enc->header_for(*packet, seq++), packet->annexb);
//   enc->request_keyframe();                                 // the viewer lost a frame, or a new viewer
//
// Low latency (CODECAPI_AVLowLatencyMode), no B-frames, and a long GOP: a keyframe at the start and
// then only every `keyframe_interval_s` or when asked, so a still picture costs almost nothing. Hardware
// encoders are async MFTs: a thread of its own answers their events and feeds them a picture for every
// "need input".
#pragma once

#include <d3d11.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "kvm/gpu_device.h"
#include "kvm/protocol.h"
#include "kvm/queue.h"

namespace kvm {

class H264Encoder {
public:
    struct Options {
        int width = 0;   // even
        int height = 0;  // even
        int fps = 30;
        int bitrate = 8'000'000;  // bits per second, constant bitrate
        int keyframe_interval_s = 60;
    };
    struct Packet {
        std::vector<std::uint8_t> annexb;  // one access unit, with start codes
        bool keyframe = false;
        std::int64_t t_capture_us = 0;  // as given to encode()
        std::int64_t t_encoded_us = 0;  // steady_now_us() when it came out of the encoder
    };

    // The hardware H.264 encoder on `gpu`'s adapter. nullptr with `why` set if there is none or it
    // won't take these settings.
    static std::unique_ptr<H264Encoder> create(std::shared_ptr<GpuDevice> gpu, const Options& options,
                                               std::string& why);
    ~H264Encoder();
    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // Queues one NV12 picture of the configured size (copied, so `nv12` may change right after).
    // Returns at once; false if the encoder has failed. If the encoder falls behind, the older
    // waiting picture is dropped: the newest one is what counts.
    bool encode(ID3D11Texture2D* nv12, std::int64_t t_capture_us);
    // Encoded access units, in order. Closed when the encoder stops or fails.
    BlockingQueue<Packet>& packets() { return packets_; }
    // The next picture given to encode() becomes a keyframe (IDR). If that one is dropped while the
    // encoder is busy, the newer picture that replaces it does.
    void request_keyframe() { keyframe_requested_ = true; }
    // Changes the bitrate from the next picture on.
    void set_bitrate(int bits_per_second);
    // Waits up to `timeout` until every queued picture has come out: its packet is in packets() then.
    bool flush(std::chrono::milliseconds timeout);

    // The video channel header for `packet` (codec "h264", docs/PROTOCOL.md). `seq` counts the stream's frames.
    proto::FrameHeader header_for(const Packet& packet, std::uint32_t seq) const {
        return {.seq = seq,
                .t_capture_us = packet.t_capture_us,
                .width = options_.width,
                .height = options_.height,
                .codec = "h264",
                .keyframe = packet.keyframe};
    }

    // Pictures dropped because the encoder was still busy with older ones, and pictures the encoder
    // itself skipped (an empty output).
    std::uint64_t dropped() const { return dropped_; }
    std::uint64_t skipped() const { return empty_; }

    const std::string& name() const { return name_; }  // e.g. "Intel® Quick Sync Video H.264 Encoder MFT"

private:
    H264Encoder() = default;
    void event_loop();
    void feed_locked();    // a waiting picture for each "need input" credit
    void drain_output();   // METransformHaveOutput
    void fail(const std::string& why);
    bool set_codec_value(const GUID& key, std::uint32_t value);

    std::shared_ptr<GpuDevice> gpu_;
    Options options_;
    std::string name_;
    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> events_;
    Microsoft::WRL::ComPtr<ICodecAPI> codec_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager_;
    Microsoft::WRL::ComPtr<IMFActivate> activate_;
    bool started_mf_ = false;
    bool provides_samples_ = true;
    std::uint32_t output_size_ = 0;
    std::atomic<std::int64_t> time_base_us_{-1};  // the first picture's capture time: sample times start at 0

    std::mutex mutex_;  // the transform calls and the queues below
    struct Waiting {
        Microsoft::WRL::ComPtr<IMFSample> sample;
        bool keyframe = false;
    };
    std::deque<Waiting> waiting_;
    int need_input_ = 0;
    std::uint64_t in_flight_ = 0;  // fed, not yet out
    std::condition_variable flushed_;
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> empty_{0};
    std::atomic<bool> keyframe_requested_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> stopping_{false};
    BlockingQueue<Packet> packets_;
    std::thread thread_;
};

}  // namespace kvm
