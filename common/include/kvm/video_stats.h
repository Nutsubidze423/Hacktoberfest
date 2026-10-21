// SPDX-License-Identifier: Apache-2.0

// What arrives on the video channel and what gets shown (roadmap P2.1): the numbers behind the
// viewer's HUD, a log line every few seconds, and `--stats <file.csv>` with one row per frame shown,
// which tools/video_stats_report.cpp turns into docs/PERF.md rows. One per process (video_stats());
// thread-safe, since the read, decode and UI threads all report to it.
//
//   kvm::video_stats().received("window", payload_bytes);           // every frame that arrives
//   kvm::video_stats().shown({.layer = "window", .seq = ..., ...});  // once it is on screen
//   const auto s = kvm::video_stats().summary("window");             // the last second, for the HUD
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kvm {

// A frame that is now on screen.
struct ShownFrame {
    std::string_view layer;    // "window" (the viewer) or "desk"
    std::uint64_t stream = 0;  // desk mode: the window it shows; each stream numbers its own frames
    std::uint32_t seq = 0;
    std::string_view codec;
    std::size_t bytes = 0;  // its payload on the channel
    int width = 0;
    int height = 0;
    std::optional<std::int64_t> latency_us;  // captured on the other PC -> on screen here
    std::int64_t decode_us = 0;
    std::int64_t present_us = 0;  // decoded -> on screen
};

struct StatsSummary {
    std::uint64_t frames = 0;  // shown
    double fps = 0;
    double mbps = 0;  // video payload received
    std::optional<double> latency_p50_ms;
    std::optional<double> latency_p95_ms;
    std::optional<double> decode_ms;   // average
    std::optional<double> present_ms;  // average
    std::uint64_t dropped = 0;         // frame numbers never shown (gaps)
    std::uint64_t repeated = 0;        // a frame shown again (the sender repeats the last one every 500 ms)
};

class VideoStats {
public:
    ~VideoStats();

    // From now on, writes one row per frame shown to `path`, after a header. False if it can't open it.
    bool open_csv(const std::filesystem::path& path);
    void received(std::string_view layer, std::size_t bytes);
    void shown(const ShownFrame& frame);
    // Over the last `window`, for one layer ("" = all of them).
    StatsSummary summary(std::string_view layer = {}, std::chrono::milliseconds window = std::chrono::seconds(1));
    // "video: window 30 fps, ..." for the log, or empty if nothing was shown in the last `window`.
    std::string log_line(std::chrono::milliseconds window = std::chrono::seconds(5));

private:
    struct Sample {
        std::int64_t at_us = 0;
        std::string layer;
        std::optional<std::int64_t> latency_us;
        std::int64_t decode_us = 0;
        std::int64_t present_us = 0;
        std::uint64_t gap = 0;  // frame numbers skipped before this one
        bool repeat = false;
    };
    struct Arrival {
        std::int64_t at_us = 0;
        std::string layer;
        std::size_t bytes = 0;
    };
    void trim_locked(std::int64_t now_us);

    std::mutex mutex_;
    std::deque<Sample> samples_;
    std::deque<Arrival> arrivals_;
    std::map<std::pair<std::string, std::uint64_t>, std::uint32_t> last_seq_;  // per layer and stream
    std::ofstream csv_;
    std::int64_t csv_start_us_ = 0;
    int csv_unflushed_ = 0;
};

// The process's one instance.
VideoStats& video_stats();

// What a `--stats` CSV says, per layer (tools/video_stats_report).
struct CsvReport {
    std::string layer;
    std::uint64_t frames = 0;
    double seconds = 0;  // first to last frame
    double fps = 0;
    double mbps = 0;  // the frames shown
    std::optional<double> latency_p50_ms;
    std::optional<double> latency_p95_ms;
    std::optional<double> latency_p99_ms;
    std::optional<double> latency_max_ms;
    double decode_avg_ms = 0;
    double decode_p95_ms = 0;
    double present_avg_ms = 0;
    double present_p95_ms = 0;
    std::uint64_t dropped = 0;
    std::uint64_t repeated = 0;
};
// Empty if the file can't be read or has no rows.
std::vector<CsvReport> report_csv(const std::filesystem::path& path);

}  // namespace kvm
