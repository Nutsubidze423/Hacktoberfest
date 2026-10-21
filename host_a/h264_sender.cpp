// SPDX-License-Identifier: Apache-2.0

#include "h264_sender.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

#include "kvm/bitrate_control.h"
#include "kvm/log.h"
#include "kvm/net.h"  // steady_now_us

namespace kvm {

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kLogEvery = std::chrono::seconds(5);
constexpr auto kPollStop = std::chrono::milliseconds(100);
// A still window: the last picture is sent again this often, so the viewer doesn't call it stalled.
constexpr auto kRepeatAfter = std::chrono::milliseconds(500);
// How often the bitrate is looked at (BitrateControl::tick).
constexpr auto kBitrateTick = std::chrono::milliseconds(500);

}  // namespace

std::unique_ptr<H264Sender> H264Sender::create(std::shared_ptr<GpuDevice> gpu, int width, int height,
                                               VideoSender::SendFn send, Options options, std::string& why) {
    if (!gpu || !gpu->has_video()) {
        why = "h264: no GPU with video support";
        return nullptr;
    }
    auto s = std::unique_ptr<H264Sender>(new H264Sender());
    s->gpu_ = std::move(gpu);
    s->send_ = std::move(send);
    s->options_ = options;
    s->bitrate_ = options.bitrate;
    s->target_ = options.bitrate;
    s->nv12_ = std::make_unique<Nv12Converter>(s->gpu_);
    s->encoder_ = H264Encoder::create(s->gpu_,
                                      {.width = width & ~1,
                                       .height = height & ~1,
                                       .fps = std::max(options.max_fps, 1),
                                       .bitrate = options.bitrate,
                                       .keyframe_interval_s = options.keyframe_interval_s},
                                      why);
    if (!s->encoder_) return nullptr;
    s->feed_thread_ = std::jthread([p = s.get()](std::stop_token stop) { p->feed_loop(stop); });
    s->send_thread_ = std::jthread([p = s.get()](std::stop_token stop) { p->send_loop(stop); });
    return s;
}

H264Sender::~H264Sender() {
    stop();
}

void H264Sender::request_keyframe() {
    std::lock_guard lock(mutex_);
    const auto now = Clock::now();
    if (now - last_keyframe_request_ < kMinKeyframeGap) return;
    last_keyframe_request_ = now;
    if (encoder_) encoder_->request_keyframe();
}

void H264Sender::stop() {
    feed_thread_.request_stop();
    send_thread_.request_stop();
    frames_.close();
    {
        std::lock_guard lock(mutex_);
        if (encoder_) encoder_->packets().close();  // wakes the send thread
    }
    encoder_changed_.notify_all();
    if (feed_thread_.joinable()) feed_thread_.join();
    if (send_thread_.joinable()) send_thread_.join();
    // A new encoder half made (follow_bitrate) uses this object: let it finish before anything goes.
    if (next_encoder_.valid()) next_encoder_.wait();
}

// The encoder for frames of this size: a new one (and stream) when the size changed.
std::shared_ptr<H264Encoder> H264Sender::encoder_for(int width, int height) {
    width &= ~1;
    height &= ~1;
    std::unique_lock lock(mutex_);
    if (encoder_ && !encoder_->failed() && encoder_->width() == width && encoder_->height() == height) {
        return encoder_;
    }
    if (!encoder_ && width == refused_w_ && height == refused_h_) return nullptr;  // tried: no encoder
    if (encoder_) encoder_->packets().close();  // the send thread finishes its packets, then waits
    lock.unlock();
    std::string why;
    auto next = std::shared_ptr<H264Encoder>(H264Encoder::create(gpu_,
                                                                 {.width = width,
                                                                  .height = height,
                                                                  .fps = std::max(options_.max_fps, 1),
                                                                  .bitrate = bitrate_,
                                                                  .keyframe_interval_s = options_.keyframe_interval_s},
                                                                 why));
    lock.lock();
    if (!next) {
        log::warn("video: {}", why);
        refused_w_ = width;
        refused_h_ = height;
    }
    encoder_ = next;
    encoder_changed_.notify_all();
    return next;
}

void H264Sender::follow_bitrate(int target) {
    target_ = target;
    // A new encoder made in the background is ready: it takes over (its stream starts with a keyframe).
    if (next_encoder_.valid() && next_encoder_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto next = next_encoder_.get();
        // Made for an old wish: the target has since moved the other way, so it would be a step back.
        const bool stale = next && (next_bitrate_ > bitrate_) != (target > bitrate_);
        if (next && !stale) {
            std::lock_guard lock(mutex_);
            if (encoder_ && encoder_->width() == next->width() && encoder_->height() == next->height()) {
                log::info("video: h264 bitrate {:.1f} -> {:.1f} Mbit/s (a new encoder)", bitrate_.load() / 1e6,
                          next_bitrate_ / 1e6);
                encoder_->packets().close();  // the send thread sends what it has, then takes the new one
                encoder_ = std::move(next);
                bitrate_ = next_bitrate_;
                encoder_changed_.notify_all();
            }
        }
        last_switch_ = Clock::now();
    }
    std::shared_ptr<H264Encoder> current;
    {
        std::lock_guard lock(mutex_);
        current = encoder_;
    }
    const int now = bitrate_;
    if (!current || next_encoder_.valid() || (target > now * 4 / 5 && target < now * 5 / 4)) return;
    if (current->bitrate_modifiable()) {
        current->set_bitrate(target);
        bitrate_ = target;
        return;
    }
    // Down as soon as the last switch is done; up at most once per kSwitchGap.
    if (target > now && Clock::now() - last_switch_ < kSwitchGap) return;
    // Rounded to 100 kbit/s, made while the current encoder goes on.
    next_bitrate_ = (target + 50'000) / 100'000 * 100'000;
    next_encoder_ =
        std::async(std::launch::async, [this, w = current->width(), h = current->height(), bitrate = next_bitrate_] {
            std::string why;
            auto made =
                std::shared_ptr<H264Encoder>(H264Encoder::create(gpu_,
                                                                 {.width = w,
                                                                  .height = h,
                                                                  .fps = std::max(options_.max_fps, 1),
                                                                  .bitrate = bitrate,
                                                                  .keyframe_interval_s = options_.keyframe_interval_s},
                                                                 why));
            if (!made) log::warn("video: {}", why);
            return made;
        });
}

void H264Sender::feed_loop(std::stop_token stop) {
    const auto min_interval =
        options_.max_fps > 0 ? std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(1)) / options_.max_fps
                             : Clock::duration::zero();
    auto next_send = Clock::now();
    std::optional<GpuFrame> last;  // the last frame encoded, encoded again while the window is still
    auto last_fed = Clock::now();
    auto period_start = Clock::now();
    std::uint64_t sent_before = 0, bytes_before = 0, skipped_before = frames_.overwritten();
    BitrateControl control({.max = options_.bitrate, .min = options_.min_bitrate});
    auto next_tick = Clock::now() + kBitrateTick;

    while (!stop.stop_requested() && !channel_closed_) {
        auto frame = frames_.take_for(kPollStop);
        if (frame) {
            // Respect max_fps: wait for our turn, then encode the newest capture, not the one we hold.
            if (Clock::now() < next_send) {
                std::this_thread::sleep_until(next_send);
                if (auto newer = frames_.try_take()) frame = std::move(newer);
            }
            next_send = std::max(next_send + min_interval, Clock::now());
        } else if (frames_.closed()) {
            break;
        } else if (last && Clock::now() - last_fed >= kRepeatAfter) {
            frame = *last;  // nothing changed on the window: the same picture again, stamped now
            frame->t_capture_us = steady_now_us();
        }
        if (frame) {
            const auto encoder = encoder_for(frame->width, frame->height);
            ID3D11Texture2D* nv12 =
                encoder ? nv12_->convert(frame->texture.Get(), frame->width, frame->height) : nullptr;
            if (nv12) encoder->encode(nv12, frame->t_capture_us);
            last = std::move(frame);
            last_fed = Clock::now();
        }

        if (Clock::now() >= next_tick) {
            next_tick += kBitrateTick;
            const int latency = viewer_latency_ms_.exchange(-1);
            // What the link carried while send was busy. Only when send took a good part of the tick:
            // a link with room to spare returns at once and says nothing about its speed.
            const auto bytes = send_bytes_.exchange(0), us = send_us_.exchange(0);
            std::optional<int> link_bps;
            if (us > std::chrono::duration_cast<std::chrono::microseconds>(kBitrateTick).count() / 4) {
                link_bps = static_cast<int>(bytes * 8 * 1'000'000 / us);
            }
            follow_bitrate(control.tick(steady_now_us() / 1000, longest_wait_ms_.exchange(0),
                                        latency >= 0 ? std::optional(latency) : std::nullopt, link_bps));
        }

        const auto elapsed = Clock::now() - period_start;
        if (elapsed >= kLogEvery) {
            const double seconds = std::chrono::duration<double>(elapsed).count();
            const std::uint64_t sent = sent_, bytes = bytes_, skipped = frames_.overwritten();
            std::uint64_t encoder_dropped = 0;
            {
                std::lock_guard lock(mutex_);
                if (encoder_) encoder_dropped = encoder_->dropped();
            }
            log::info(
                "video: h264 {:.1f} fps, {:.1f} Mbit/s (encoder at {:.1f}, wanted {:.1f}), {} frames skipped, "
                "{} dropped by a busy encoder, {} not sent (late)",
                static_cast<double>(sent - sent_before) / seconds,
                static_cast<double>(bytes - bytes_before) * 8.0 / seconds / 1e6, bitrate_.load() / 1e6,
                target_.load() / 1e6, skipped - skipped_before, encoder_dropped, skipped_.load());
            period_start = Clock::now();
            sent_before = sent;
            bytes_before = bytes;
            skipped_before = skipped;
        }
    }
}

void H264Sender::send_loop(std::stop_token stop) {
    std::uint32_t seq = 0;  // continues across encoders: the viewer sees one stream
    std::shared_ptr<H264Encoder> encoder;
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock(mutex_);
            encoder_changed_.wait(lock, [&] { return stop.stop_requested() || (encoder_ && encoder_ != encoder); });
            if (stop.stop_requested()) return;
            encoder = encoder_;
        }
        bool skipping = false;  // a picture came too late: until the next keyframe, none is sent
        while (auto packet = encoder->packets().pop()) {
            const auto waited_ms = static_cast<int>((steady_now_us() - packet->t_encoded_us) / 1000);
            int longest = longest_wait_ms_.load();
            while (waited_ms > longest && !longest_wait_ms_.compare_exchange_weak(longest, waited_ms)) {}
            if (packet->keyframe) {
                skipping = false;
            } else if (skipping || waited_ms > kSkipAfter.count()) {
                if (!skipping) {
                    log::info("video: h264 picture waited {} ms to be sent: skipping to a keyframe", waited_ms);
                    encoder->request_keyframe();  // not rate-limited: this is ours, not the viewer's
                }
                skipping = true;
                ++seq;  // the gap tells the viewer to wait for the keyframe
                ++skipped_;
                continue;
            }
            const std::int64_t send_start = steady_now_us();
            const bool ok = send_(encoder->header_for(*packet, seq++), packet->annexb);
            send_us_ += steady_now_us() - send_start;
            send_bytes_ += static_cast<std::int64_t>(packet->annexb.size());
            if (!ok) {
                log::info("video: channel closed after {} h264 frames", sent_.load());
                channel_closed_ = true;
                return;
            }
            ++sent_;
            bytes_ += packet->annexb.size();
        }
    }
}

}  // namespace kvm
