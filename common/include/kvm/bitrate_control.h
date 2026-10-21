// SPDX-License-Identifier: Apache-2.0

// The H.264 sender's bitrate (roadmap P2.6): down quickly when the link can't keep up, back up slowly
// when it can. Fed once per tick (H264Sender: every 500 ms) with two signs of a link that is full:
//   - backlog_ms: how long the oldest encoded picture waited before the channel took it. The
//     channel's send blocks once TCP's buffers are full.
//   - viewer_latency_ms: the viewer's capture-to-screen latency (p50 of the last second, from its
//     video_stats message). Large TCP buffers hide a full link from the sender for a while; the
//     viewer sees the pictures arrive later and later.
//
//   kvm::BitrateControl control({.max = 8'000'000});
//   const int bitrate = control.tick(now_ms, backlog_ms, viewer_latency_ms, link_bps);
//
// link_bps, when known: what the link carried while the channel's send was busy (bytes sent / time
// spent in send). On a full link that is the link's speed, so the bitrate goes straight to a bit
// under it instead of stepping down one x0.7 at a time.
//
// Down: x0.7 per tick while either sign is up, to no less than `min`. Up: x1.15 per tick after
// `calm_ms` without one, to no more than `max`. A backlog counts from its second tick in a row: one
// alone is a burst (a keyframe), not a full link. The latency sign compares against the lowest
// latency of the last 10 s, so a link that is slow but steady doesn't count as full. Header-only;
// not thread-safe.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace kvm {

class BitrateControl {
public:
    struct Options {
        int max = 8'000'000;  // bits per second: the configured bitrate
        int min = 1'000'000;
        double down = 0.7;
        double up = 1.15;
        std::int64_t calm_ms = 1500;  // without a sign of a full link, before going up again
        int backlog_limit_ms = 100;   // a picture waited longer than this to be sent
        int latency_rise_ms = 80;     // the viewer's latency rose this much above its recent lowest
    };

    explicit BitrateControl(Options options) : options_(options), bitrate_(options.max) {}

    // One tick: the bitrate to encode at from now on.
    int tick(std::int64_t now_ms, int backlog_ms, std::optional<int> viewer_latency_ms,
             std::optional<int> link_bps = std::nullopt) {
        // The lowest viewer latency of the last 10 s: what this link does when it isn't full.
        if (viewer_latency_ms) latencies_.emplace_back(now_ms, *viewer_latency_ms);
        while (!latencies_.empty() && latencies_.front().first < now_ms - kBaselineMs) latencies_.pop_front();
        std::optional<int> baseline;
        for (const auto& [at, ms] : latencies_) baseline = baseline ? std::min(*baseline, ms) : ms;

        backlog_ticks_ = backlog_ms > options_.backlog_limit_ms ? backlog_ticks_ + 1 : 0;
        const bool backlog = backlog_ticks_ >= 2;
        const bool late = viewer_latency_ms && baseline && *viewer_latency_ms > *baseline + options_.latency_rise_ms;
        if (backlog || late) {
            int next = static_cast<int>(std::lround(bitrate_ * options_.down));
            if (link_bps) next = std::min(next, static_cast<int>(std::lround(*link_bps * kLinkShare)));
            bitrate_ = std::max(options_.min, next);
            last_full_ms_ = now_ms;
        } else if (!last_full_ms_ || now_ms - *last_full_ms_ >= options_.calm_ms) {
            bitrate_ = std::min(options_.max, static_cast<int>(std::lround(bitrate_ * options_.up)));
        }
        return bitrate_;
    }

    int bitrate() const { return bitrate_; }

private:
    static constexpr std::int64_t kBaselineMs = 10'000;
    static constexpr double kLinkShare = 0.8;  // of a full link's measured speed: room to drain the backlog

    Options options_;
    int bitrate_;
    std::optional<std::int64_t> last_full_ms_;
    int backlog_ticks_ = 0;  // ticks in a row with a backlog
    std::deque<std::pair<std::int64_t, int>> latencies_;  // (when, viewer latency ms)
};

}  // namespace kvm
