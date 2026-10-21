// SPDX-License-Identifier: Apache-2.0

// video_stats_report: what a `--stats <file.csv>` run measured (kvm/video_stats.h), per layer, as a
// docs/PERF.md table row.
//
//   kvm_client.exe ... --stats run.csv        (or kvm_host.exe: the PC that shows the frames)
//   video_stats_report.exe run.csv [label]    (label: the content, e.g. "browser scrolling")

#include <cstdio>
#include <filesystem>
#include <format>
#include <optional>
#include <string>

#include "kvm/video_stats.h"

namespace {

std::string ms(std::optional<double> v) {
    return v ? std::format("{:.0f}", *v) : std::string("—");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: video_stats_report.exe <file.csv> [label]\n");
        return 2;
    }
    const std::filesystem::path csv = argv[1];
    std::string label;
    if (argc > 2) {
        for (const wchar_t c : std::wstring_view(argv[2])) label.push_back(static_cast<char>(c));
    }
    const auto reports = kvm::report_csv(csv);
    if (reports.empty()) {
        std::printf("no frames in %ls\n", argv[1]);
        return 1;
    }
    std::printf(
        "| Content | Layer | Frames | fps | Mbit/s | Latency p50 / p95 / p99 / max (ms) | Decode avg / p95 (ms) "
        "| Present avg / p95 (ms) | Dropped | Repeated |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|\n");
    for (const auto& r : reports) {
        std::printf(
            "%s\n",
            std::format("| {} | {} | {} in {:.0f} s | {:.1f} | {:.2f} | {} / {} / {} / {} | {:.1f} / {:.1f} | "
                        "{:.1f} / {:.1f} | {} | {} |",
                        label.empty() ? std::string("—") : label, r.layer, r.frames, r.seconds, r.fps, r.mbps,
                        ms(r.latency_p50_ms), ms(r.latency_p95_ms), ms(r.latency_p99_ms), ms(r.latency_max_ms),
                        r.decode_avg_ms, r.decode_p95_ms, r.present_avg_ms, r.present_p95_ms, r.dropped, r.repeated)
                .c_str());
    }
    return 0;
}
