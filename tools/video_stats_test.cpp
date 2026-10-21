// SPDX-License-Identifier: Apache-2.0

// video_stats_test: kvm/video_stats.h (roadmap P2.1). Checks:
//   - the last-second summary: frames, latency p50 / p95 (nearest rank), decode and present averages;
//   - gaps in the frame numbers count as dropped, a frame shown again as repeated, per stream;
//   - a layer filter, and received bytes as Mbit/s;
//   - the --stats CSV: one row per frame, and report_csv() reads back the same numbers.
//
//   video_stats_test.exe   (exit code 0 = all passed)

#include <windows.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <string_view>
#include <thread>

#include "kvm/log.h"
#include "kvm/video_stats.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

struct Checks {
    int failed = 0;
    void operator()(bool ok, std::string_view what) {
        kvm::log::info("{} {}", ok ? "[ok]  " : "[FAIL]", what);
        if (!ok) ++failed;
    }
};

bool close_to(std::optional<double> v, double want) {
    return v && std::abs(*v - want) < 0.01;
}

}  // namespace

int main() {
    Checks check;

    kvm::log::info("the last second");
    {
        kvm::VideoStats stats;
        // 20 frames, latencies 10, 20, ..., 200 ms; decode 5 ms, present 2 ms.
        for (std::uint32_t i = 1; i <= 20; ++i) {
            stats.received("window", 1000);
            stats.shown({.layer = "window",
                         .seq = i,
                         .codec = "jpeg",
                         .bytes = 1000,
                         .width = 1920,
                         .height = 1080,
                         .latency_us = i * 10'000,
                         .decode_us = 5000,
                         .present_us = 2000});
        }
        const auto s = stats.summary("window");
        check(s.frames == 20 && close_to(s.fps, 20), std::format("20 frames, {:.1f} fps", s.fps));
        check(close_to(s.latency_p50_ms, 100) && close_to(s.latency_p95_ms, 190),
              "latency p50 / p95 by nearest rank: 100 / 190 ms");
        check(close_to(s.decode_ms, 5) && close_to(s.present_ms, 2), "decode and present averages");
        check(close_to(s.mbps, 0.16), std::format("received 20 x 1000 bytes = {:.2f} Mbit/s", s.mbps));
        check(s.dropped == 0 && s.repeated == 0, "nothing dropped or repeated");
        check(stats.summary("desk").frames == 0, "the desk layer is counted apart");
        check(stats.log_line().starts_with("video: window"), "a log line while frames are shown");
    }

    kvm::log::info("dropped and repeated");
    {
        kvm::VideoStats stats;
        for (const std::uint32_t seq : {1u, 2u, 2u, 5u, 6u}) stats.shown({.layer = "window", .seq = seq});
        // Desk mode: two windows, each with its own numbers.
        for (const std::uint32_t seq : {1u, 2u, 3u}) stats.shown({.layer = "desk", .stream = 7, .seq = seq});
        for (const std::uint32_t seq : {1u, 4u}) stats.shown({.layer = "desk", .stream = 8, .seq = seq});
        stats.shown({.layer = "window", .seq = 1});  // a new stream starts over: neither
        const auto w = stats.summary("window");
        check(w.repeated == 1 && w.dropped == 2,
              std::format("window: {} repeated, {} dropped (2 and 2; 3 and 4 missing)", w.repeated, w.dropped));
        const auto d = stats.summary("desk");
        check(d.dropped == 2 && d.repeated == 0, "desk: per window, only window 8's 2 and 3 are missing");
        check(!w.latency_p50_ms, "no latency while the clock offset is unknown");
    }

    kvm::log::info("the CSV");
    {
        const fs::path csv =
            fs::temp_directory_path() / std::format(L"sarke_video_stats_{}.csv", GetCurrentProcessId());
        {
            kvm::VideoStats stats;
            check(stats.open_csv(csv), "--stats opens the file");
            for (std::uint32_t i = 1; i <= 21; ++i) {
                if (i == 10) continue;  // one never arrives
                stats.shown({.layer = "desk",
                             .stream = 3,
                             .seq = i,
                             .codec = "jpeg",
                             .bytes = 50'000,
                             .width = 800,
                             .height = 600,
                             .latency_us = 20'000 + i * 1000,
                             .decode_us = 4000,
                             .present_us = 1000});
                std::this_thread::sleep_for(10ms);
            }
        }  // flushed when it goes
        const auto reports = kvm::report_csv(csv);
        check(reports.size() == 1 && reports[0].layer == "desk" && reports[0].frames == 20, "20 rows read back");
        if (!reports.empty()) {
            const auto& r = reports[0];
            check(close_to(r.latency_p50_ms, 31) && close_to(r.latency_max_ms, 41) && close_to(r.latency_p99_ms, 41),
                  std::format("latency p50 {} / p99 {} / max {} ms", r.latency_p50_ms.value_or(-1.0),
                              r.latency_p99_ms.value_or(-1.0), r.latency_max_ms.value_or(-1.0)));
            check(close_to(r.decode_avg_ms, 4) && close_to(r.present_avg_ms, 1) && r.dropped == 1 && r.repeated == 0,
                  "decode, present, and the one dropped frame");
            check(r.seconds > 0.1 && r.fps > 10 && r.mbps > 0,
                  std::format("{:.0f} fps over {:.2f} s", r.fps, r.seconds));
        }
        std::error_code ec;
        fs::remove(csv, ec);
    }

    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
