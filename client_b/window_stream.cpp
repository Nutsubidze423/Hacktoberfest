// SPDX-License-Identifier: Apache-2.0

#include "window_stream.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "h264_decoder.h"
#include "jpeg_decoder.h"
#include "kvm/annexb.h"
#include "kvm/log.h"
#include "picture_converter.h"

namespace kvm {

WindowStream::WindowStream(ViewerWindow& viewer, std::function<void()> request_keyframe)
    : viewer_(viewer), request_keyframe_(std::move(request_keyframe)) {
    jpeg_thread_ = std::jthread([this] { jpeg_loop(); });
    h264_thread_ = std::jthread([this] { h264_loop(); });
}

WindowStream::~WindowStream() {
    close();
    // jthread members join here.
}

void WindowStream::push(net::Frame frame) {
    if (frame.header.codec == "h264") {
        h264_.push(std::move(frame));  // full: the oldest goes, and the gap waits for a keyframe
    } else {
        latest_.put(std::move(frame));
    }
}

void WindowStream::prepare(const std::string& codec) {
    if (codec == "h264") prepare_h264_ = true;  // the H.264 thread looks at it within 100 ms
}

void WindowStream::close() {
    latest_.close();
    h264_.close();
}

std::optional<std::int64_t> WindowStream::local_time(std::int64_t t_capture_us) const {
    // t_capture_us is the owner's clock; subtracting the offset maps it to ours (the same as
    // Connection::peer_to_local_us).
    if (!has_clock_offset_) return std::nullopt;
    return t_capture_us - clock_offset_us_;
}

void WindowStream::jpeg_loop() {
    JpegDecoder decoder;
    BgraImage spare;  // reused: present() hands back a spare buffer
    bool codec_warned = false;
    std::uint64_t decoded_frames = 0;

    while (auto frame = latest_.take()) {
        if (frame->header.codec != "jpeg") {
            if (!codec_warned) {
                log::info("video: codec '{}' is not shown (counted for MBPS only)", frame->header.codec);
                codec_warned = true;
            }
            continue;
        }
        const std::int64_t started = steady_now_us();
        if (!decoder.decode(frame->payload, spare)) {
            log::warn("video: frame {} dropped: {}", frame->header.seq, decoder.last_error());
            continue;
        }
        const std::int64_t decoded = steady_now_us();
        FrameInfo info{.seq = frame->header.seq,
                       .t_capture_local_us = local_time(frame->header.t_capture_us),
                       .bytes = frame->payload.size(),
                       .decode_us = decoded - started,
                       .decoded_at_us = decoded};
        viewer_.present(spare, info);
        ++decoded_frames;
    }
    if (decoded_frames > 0) log::info("video: jpeg stopped after {} decoded frames", decoded_frames);
}

void WindowStream::h264_loop() {
    std::shared_ptr<GpuDevice> gpu;  // the viewer's: the decoder and converter work on it
    std::unique_ptr<H264Decoder> decoder;
    std::unique_ptr<PictureConverter> converter;
    BgraImage spare;
    std::optional<std::uint32_t> last_seq;
    bool need_keyframe = true;
    bool decoder_missing = false;
    std::uint64_t decoded = 0, waited = 0;
    std::chrono::steady_clock::time_point last_request{};

    // The decoder on the viewer's current device (a new one after the device was lost).
    const auto ensure_decoder = [&] {
        const auto now = viewer_.gpu();
        if (now == gpu && (decoder || decoder_missing)) return false;
        gpu = now;
        converter.reset();
        std::string why;
        decoder = H264Decoder::create(gpu && gpu->has_video() ? gpu : nullptr, why);
        decoder_missing = !decoder;
        if (!decoder) log::error("video: {}: h264 frames are not shown", why);
        converter = std::make_unique<PictureConverter>(gpu);
        return decoder != nullptr;  // a new decoder: it starts at a keyframe
    };
    // Waiting for a keyframe: ask the owner for one, now and then.
    const auto ask_for_keyframe = [&] {
        const auto now = std::chrono::steady_clock::now();
        if (!request_keyframe_ || now - last_request < kKeyframeRequestGap) return;
        last_request = now;
        request_keyframe_();
    };

    for (;;) {
        auto frame = h264_.pop_for(std::chrono::milliseconds(100));
        if (!frame) {
            if (h264_.closed()) break;
            if (prepare_h264_.exchange(false)) ensure_decoder();  // proto::VideoConfig: be ready
            continue;
        }
        const proto::FrameHeader& h = frame->header;
        // A missing frame: the ones after it refer to a picture that isn't there.
        if (last_seq && h.seq != *last_seq + 1 && !need_keyframe) {
            log::info("video: h264 frame {} after {}: waiting for a keyframe", h.seq, *last_seq);
            need_keyframe = true;
        }
        last_seq = h.seq;
        if (const auto why = annexb::check(frame->payload, h.keyframe); !why.empty()) {
            log::warn("video: h264 frame {} dropped: {}", h.seq, why);
            need_keyframe = true;
            ask_for_keyframe();
            continue;
        }
        if (need_keyframe && !h.keyframe) {
            ++waited;
            ask_for_keyframe();
            continue;
        }
        if (ensure_decoder() && !h.keyframe) {  // a new decoder starts at a keyframe
            need_keyframe = true;
            ++waited;
            ask_for_keyframe();
            continue;
        }
        if (!decoder) continue;
        need_keyframe = false;

        const std::int64_t started = steady_now_us();
        auto picture = decoder->decode(frame->payload, h.t_capture_us);
        if (!picture) continue;
        ++decoded;
        if (h264_.size() > 0) continue;  // more are waiting: show the newest picture, not this one

        FrameInfo info{.seq = h.seq,
                       .t_capture_local_us = local_time(h.t_capture_us),
                       .bytes = frame->payload.size(),
                       .codec = "h264"};
        if (picture->texture) {
            auto bgra = converter->to_texture(*picture);
            const int w = picture->width, ht = picture->height;
            picture.reset();  // the decoder has its texture back
            if (!bgra) continue;
            info.decoded_at_us = steady_now_us();
            info.decode_us = info.decoded_at_us - started;
            viewer_.present_texture(std::move(bgra), w, ht, info);
        } else if (PictureConverter::to_bgra(*picture, spare)) {
            info.decoded_at_us = steady_now_us();
            info.decode_us = info.decoded_at_us - started;
            viewer_.present(spare, info);
        }
    }
    if (decoded > 0 || waited > 0) {
        log::info("video: h264 stopped after {} decoded frames ({} skipped waiting for a keyframe)", decoded, waited);
    }
}

}  // namespace kvm
