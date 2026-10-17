#include "desk_share.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <vector>

#include "jpeg_encoder.h"
#include "kvm/log.h"
#include "kvm/net.h"

namespace kvm {

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kPoll = std::chrono::milliseconds(10);  // how soon a rects-only change goes out
constexpr auto kLogEvery = std::chrono::seconds(5);

}  // namespace

DeskShare::DeskShare(const VirtualDisplay::MonitorRect& vdd, desk::Region& region, SendFn send, Options options)
    : region_(region), send_(std::move(send)), options_(options) {
    tracker_ = std::make_unique<DeskTracker>(region_);
    capture_ = ScreenCapture::for_monitor(vdd.monitor, /*capture_cursor=*/false);
    capture_->start(frames_, [this] { lost_ = true; });
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
    log::info("desk: sharing the other screen's windows ({}x{} at {},{})", vdd.rect.right - vdd.rect.left,
              vdd.rect.bottom - vdd.rect.top, vdd.rect.left, vdd.rect.top);
}

DeskShare::~DeskShare() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
    if (capture_) capture_->stop();
    frames_.close();
    tracker_.reset();
    log::info("desk: stopped after {} frames", sent_.load());
}

void DeskShare::run(std::stop_token stop) {
    JpegEncoder jpeg(options_.jpeg_quality);
    std::vector<std::uint8_t> encoded;
    const auto min_interval = options_.max_fps > 0
                                  ? std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(1)) / options_.max_fps
                                  : Clock::duration::zero();
    std::optional<CapturedFrame> full;  // the newest whole VDD picture
    bool fresh = false;                 // `full` changed since the last send
    std::uint64_t sent_version = ~0ull;
    bool empty_sent = false;
    std::uint32_t seq = 0;
    auto next_send = Clock::now();

    auto period_start = Clock::now();
    std::uint64_t period_frames = 0, period_bytes = 0, period_pixels = 0;
    double period_encode_ms = 0;

    while (!stop.stop_requested()) {
        if (auto frame = frames_.take_for(kPoll)) {
            full = std::move(*frame);
            fresh = true;
        } else if (frames_.closed()) {
            break;
        }

        if (fresh || region_.version() != sent_version) {
            // Respect max_fps, then send the newest picture and rects, not the ones we hold.
            if (Clock::now() < next_send) {
                std::this_thread::sleep_until(next_send);
                if (auto newer = frames_.try_take()) full = std::move(*newer);
            }
            next_send = std::max(next_send + min_interval, Clock::now());
            fresh = false;
            sent_version = region_.version();
            const auto rects = region_.rects();
            display::Rect vdd;
            bool live = false;
            const bool has_vdd = region_.vdd_rect(vdd, live);

            proto::FrameHeader header{.seq = seq, .t_capture_us = steady_now_us(), .layer = "desk"};
            header.desk_w = full ? full->width : vdd.width();
            header.desk_h = full ? full->height : vdd.height();
            // The windows, relative to the VDD and inside the captured picture; their bounding box is sent.
            display::Rect box{header.desk_w, header.desk_h, 0, 0};
            if (full && has_vdd) {
                for (const auto& r : rects) {
                    const proto::DeskRect d{
                        .x = std::max(r.left - vdd.left, 0),
                        .y = std::max(r.top - vdd.top, 0),
                        .w = std::min(r.right - vdd.left, full->width) - std::max(r.left - vdd.left, 0),
                        .h = std::min(r.bottom - vdd.top, full->height) - std::max(r.top - vdd.top, 0),
                    };
                    if (d.w <= 0 || d.h <= 0) continue;
                    header.rects.push_back(d);
                    box = {std::min(box.left, d.x), std::min(box.top, d.y), std::max(box.right, d.x + d.w),
                           std::max(box.bottom, d.y + d.h)};
                }
            }

            if (header.rects.empty()) {
                if (!empty_sent) {  // tell the peer once that there is nothing to draw
                    header.codec = "none";
                    send_(header, {});
                    ++seq;
                    empty_sent = true;
                }
            } else {
                const std::uint8_t* first =
                    full->bgra.data() + static_cast<size_t>(box.top) * full->stride + static_cast<size_t>(box.left) * 4;
                if (jpeg.encode(first, box.width(), box.height(), full->stride, encoded)) {
                    header.t_capture_us = full->t_capture_us;
                    header.width = box.width();
                    header.height = box.height();
                    header.x = box.left;
                    header.y = box.top;
                    send_(header, encoded);
                    ++seq;
                    ++sent_;
                    empty_sent = false;
                    ++period_frames;
                    period_bytes += encoded.size();
                    period_pixels += static_cast<std::uint64_t>(box.width()) * box.height();
                    period_encode_ms += jpeg.last_encode_ms();
                }
            }
        }

        const auto elapsed = Clock::now() - period_start;
        if (elapsed >= kLogEvery) {
            if (period_frames > 0) {
                const double seconds = std::chrono::duration<double>(elapsed).count();
                const double n = static_cast<double>(period_frames);
                log::info("desk video: {:.1f} fps, {:.0f}k px/frame, encode {:.1f} ms, {:.1f} Mbit/s",
                          n / seconds, static_cast<double>(period_pixels) / n / 1000.0, period_encode_ms / n,
                          static_cast<double>(period_bytes) * 8.0 / seconds / 1e6);
            }
            period_start = Clock::now();
            period_frames = period_bytes = period_pixels = 0;
            period_encode_ms = 0;
        }
    }
}

}  // namespace kvm
