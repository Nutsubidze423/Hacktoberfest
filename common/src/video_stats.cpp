// SPDX-License-Identifier: Apache-2.0

#include "kvm/video_stats.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>
#include <sstream>

#include "kvm/net.h"  // steady_now_us

namespace kvm {

namespace {

constexpr auto kKeep = std::chrono::seconds(10);  // the longest window summary() and log_line() look at
constexpr int kFlushEvery = 30;                    // CSV rows

// Nearest rank: the smallest value with at least p of them at or below it.
double percentile(std::vector<double> values, double p) {
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(values.size())));
    return values[std::clamp<std::size_t>(rank, 1, values.size()) - 1];
}

double average(const std::vector<double>& values) {
    return values.empty() ? 0 : std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

std::string ms(std::int64_t us) {
    return std::format("{:.3f}", static_cast<double>(us) / 1000.0);
}

}  // namespace

VideoStats::~VideoStats() {
    if (csv_.is_open()) csv_.flush();
}

bool VideoStats::open_csv(const std::filesystem::path& path) {
    std::lock_guard lock(mutex_);
    if (csv_.is_open()) csv_.close();
    csv_.clear();
    csv_.open(path, std::ios::trunc);
    if (!csv_) return false;
    csv_start_us_ = steady_now_us();
    csv_ << "t_ms,layer,stream,seq,codec,bytes,width,height,latency_ms,decode_ms,present_ms,gap,repeat\n";
    csv_.flush();
    return static_cast<bool>(csv_);
}

void VideoStats::flush() {
    std::lock_guard lock(mutex_);
    if (csv_.is_open()) csv_.flush();
    csv_unflushed_ = 0;
}

void VideoStats::received(std::string_view layer, std::size_t bytes) {
    const std::int64_t now = steady_now_us();
    std::lock_guard lock(mutex_);
    arrivals_.push_back({now, std::string(layer), bytes});
    trim_locked(now);
}

void VideoStats::shown(const ShownFrame& f) {
    const std::int64_t now = steady_now_us();
    std::lock_guard lock(mutex_);
    Sample s{.at_us = now,
             .layer = std::string(f.layer),
             .latency_us = f.latency_us,
             .decode_us = f.decode_us,
             .present_us = f.present_us};
    auto [it, first] = last_seq_.try_emplace({std::string(f.layer), f.stream}, f.seq);
    if (!first) {
        if (f.seq == it->second) {
            s.repeat = true;
        } else if (f.seq > it->second) {
            s.gap = f.seq - it->second - 1;
        }  // lower: the stream started over
        it->second = f.seq;
    }
    samples_.push_back(s);
    trim_locked(now);

    if (!csv_.is_open()) return;
    csv_ << ms(now - csv_start_us_) << ',' << f.layer << ',' << f.stream << ',' << f.seq << ',' << f.codec << ','
         << f.bytes << ',' << f.width << ',' << f.height << ',' << (f.latency_us ? ms(*f.latency_us) : std::string())
         << ',' << ms(f.decode_us) << ',' << ms(f.present_us) << ',' << s.gap << ',' << (s.repeat ? 1 : 0) << '\n';
    if (++csv_unflushed_ >= kFlushEvery) {
        csv_.flush();
        csv_unflushed_ = 0;
    }
}

void VideoStats::trim_locked(std::int64_t now_us) {
    const std::int64_t oldest = now_us - std::chrono::duration_cast<std::chrono::microseconds>(kKeep).count();
    while (!samples_.empty() && samples_.front().at_us < oldest) samples_.pop_front();
    while (!arrivals_.empty() && arrivals_.front().at_us < oldest) arrivals_.pop_front();
}

StatsSummary VideoStats::summary(std::string_view layer, std::chrono::milliseconds window) {
    const std::int64_t now = steady_now_us();
    const std::int64_t since = now - std::chrono::duration_cast<std::chrono::microseconds>(window).count();
    const auto in = [&](std::int64_t at, const std::string& l) { return at >= since && (layer.empty() || l == layer); };
    StatsSummary out;
    std::vector<double> latency, decode, present;
    std::size_t bytes = 0;
    {
        std::lock_guard lock(mutex_);
        trim_locked(now);
        for (const Sample& s : samples_) {
            if (!in(s.at_us, s.layer)) continue;
            ++out.frames;
            out.dropped += s.gap;
            if (s.repeat) ++out.repeated;
            if (s.latency_us) latency.push_back(static_cast<double>(*s.latency_us) / 1000.0);
            decode.push_back(static_cast<double>(s.decode_us) / 1000.0);
            present.push_back(static_cast<double>(s.present_us) / 1000.0);
        }
        for (const Arrival& a : arrivals_) {
            if (in(a.at_us, a.layer)) bytes += a.bytes;
        }
    }
    const double seconds = std::chrono::duration<double>(window).count();
    out.fps = static_cast<double>(out.frames) / seconds;
    out.mbps = static_cast<double>(bytes) * 8.0 / seconds / 1e6;
    if (!latency.empty()) {
        out.latency_p50_ms = percentile(latency, 0.50);
        out.latency_p95_ms = percentile(latency, 0.95);
    }
    if (!decode.empty()) {
        out.decode_ms = average(decode);
        out.present_ms = average(present);
    }
    return out;
}

std::string VideoStats::log_line(std::chrono::milliseconds window) {
    std::string line;
    for (const std::string_view layer : {"window", "desk"}) {
        const StatsSummary s = summary(layer, window);
        if (s.frames == 0) continue;
        if (!line.empty()) line += "; ";
        line += std::format(
            "{} {:.1f} fps, {:.1f} Mbit/s, latency {} ms, decode {:.1f} ms, present {:.1f} ms, "
            "{} dropped, {} repeated",
            layer, s.fps, s.mbps,
            s.latency_p50_ms ? std::format("p50 {:.0f} p95 {:.0f}", *s.latency_p50_ms, *s.latency_p95_ms)
                             : std::string("unknown"),
            s.decode_ms.value_or(0), s.present_ms.value_or(0), s.dropped, s.repeated);
    }
    return line.empty() ? line : "video: " + line;
}

VideoStats& video_stats() {
    static VideoStats stats;
    return stats;
}

std::vector<CsvReport> report_csv(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line)) return {};  // the header
    struct Rows {
        std::vector<double> t, latency, decode, present;
        double bytes = 0;
        std::uint64_t dropped = 0, repeated = 0;
    };
    std::map<std::string, Rows> by_layer;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream cells(line);
        for (std::string cell; std::getline(cells, cell, ',');) f.push_back(cell);
        if (line.ends_with(',')) f.emplace_back();
        if (f.size() < 13) continue;
        try {
            Rows& r = by_layer[f[1]];
            r.t.push_back(std::stod(f[0]));
            r.bytes += std::stod(f[5]);
            if (!f[8].empty()) r.latency.push_back(std::stod(f[8]));
            r.decode.push_back(std::stod(f[9]));
            r.present.push_back(std::stod(f[10]));
            r.dropped += std::stoull(f[11]);
            r.repeated += f[12] == "1" ? 1 : 0;
        } catch (const std::exception&) {
            continue;  // a cut-off last row
        }
    }
    std::vector<CsvReport> out;
    for (const auto& [layer, r] : by_layer) {
        if (r.t.empty()) continue;
        CsvReport c{.layer = layer, .frames = r.t.size()};
        c.seconds = (r.t.back() - r.t.front()) / 1000.0;
        if (c.seconds > 0) {
            c.fps = static_cast<double>(r.t.size() - 1) / c.seconds;
            c.mbps = r.bytes * 8.0 / c.seconds / 1e6;
        }
        if (!r.latency.empty()) {
            c.latency_p50_ms = percentile(r.latency, 0.50);
            c.latency_p95_ms = percentile(r.latency, 0.95);
            c.latency_p99_ms = percentile(r.latency, 0.99);
            c.latency_max_ms = *std::max_element(r.latency.begin(), r.latency.end());
        }
        c.decode_avg_ms = average(r.decode);
        c.decode_p95_ms = percentile(r.decode, 0.95);
        c.present_avg_ms = average(r.present);
        c.present_p95_ms = percentile(r.present, 0.95);
        c.dropped = r.dropped;
        c.repeated = r.repeated;
        out.push_back(std::move(c));
    }
    return out;
}

}  // namespace kvm
