#include "video_sender.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "jpeg_encoder.h"
#include "kvm/log.h"

namespace kvm {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kLogEvery = std::chrono::seconds(5);
constexpr auto kPollStop = std::chrono::milliseconds(100);

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

}  // namespace

VideoSender::VideoSender(LatestSlot<CapturedFrame>& frames, SendFn send, Options options)
    : frames_(frames),
      send_(std::move(send)),
      options_(options),
      thread_([this](std::stop_token stop) { run(stop); }) {}

VideoSender::~VideoSender() {
    stop();
}

void VideoSender::stop() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

void VideoSender::run(std::stop_token stop) {
    JpegEncoder jpeg(options_.jpeg_quality);
    std::vector<std::uint8_t> encoded;
    const auto min_interval = options_.max_fps > 0
                                  ? std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(1)) / options_.max_fps
                                  : Clock::duration::zero();
    std::uint32_t seq = 0;
    auto next_send = Clock::now();

    // Totals for the periodic log line.
    auto period_start = Clock::now();
    std::uint64_t period_frames = 0, period_bytes = 0;
    double period_encode_ms = 0, period_send_ms = 0;
    std::uint64_t skipped_before = frames_.overwritten();

    while (!stop.stop_requested()) {
        auto frame = frames_.take_for(kPollStop);
        if (frame) {
            // Respect max_fps: wait for our turn, then send the newest capture, not the one we hold.
            if (Clock::now() < next_send) {
                std::this_thread::sleep_until(next_send);
                if (auto newer = frames_.try_take()) frame = std::move(newer);
            }
            next_send = Clock::now() + min_interval;

            if (jpeg.encode(*frame, encoded)) {
                const proto::FrameHeader header{
                    .seq = seq++,
                    .t_capture_us = frame->t_capture_us,
                    .width = frame->width,
                    .height = frame->height,
                    .codec = "jpeg",
                };
                const auto send_start = Clock::now();
                if (!send_(header, encoded)) {
                    log::info("video: channel closed after {} frames", sent_.load());
                    break;
                }
                period_send_ms += ms_since(send_start);
                period_encode_ms += jpeg.last_encode_ms();
                period_bytes += encoded.size();
                ++period_frames;
                ++sent_;
            }
        } else if (frames_.closed()) {
            break;
        }

        const auto elapsed = Clock::now() - period_start;
        if (elapsed >= kLogEvery) {
            const double seconds = std::chrono::duration<double>(elapsed).count();
            const double n = static_cast<double>(std::max<std::uint64_t>(period_frames, 1));
            const std::uint64_t skipped = frames_.overwritten();
            log::info("video: {:.1f} fps, encode {:.1f} ms, send {:.1f} ms, {:.1f} Mbit/s, {} frames skipped",
                      static_cast<double>(period_frames) / seconds, period_encode_ms / n, period_send_ms / n,
                      static_cast<double>(period_bytes) * 8.0 / seconds / 1e6, skipped - skipped_before);
            period_start = Clock::now();
            period_frames = period_bytes = 0;
            period_encode_ms = period_send_ms = 0;
            skipped_before = skipped;
        }
    }
    running_ = false;
}

}  // namespace kvm
