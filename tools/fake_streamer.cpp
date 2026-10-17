// fake_streamer: stands in for A's video sender, so the B side can be tested without capture.
// Listens on the video port (50501) with Luka's kvm::net::VideoServer — the same channel A uses —
// and sends JPEGs from disk in a loop.
//
//   fake_streamer.exe <folder | image.jpg> [--port 50501] [--fps 30]
//   viewer_test.exe --connect 127.0.0.1          (same PC; or B's viewer pointed at this PC's IP)
//
// Each frame gets t_capture_us = send time, so the viewer's latency shows network + decode time.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "jpeg_decoder.h"
#include "kvm/config.h"
#include "kvm/log.h"
#include "kvm/net.h"
#include "kvm/video.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr auto kStatsInterval = std::chrono::seconds(5);

struct JpegFile {
    std::string name;
    std::vector<std::uint8_t> bytes;
    int width = 0;
    int height = 0;
};

std::string utf8(const std::wstring& s) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

bool is_jpeg(const fs::path& p) {
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return ext == L".jpg" || ext == L".jpeg";
}

// Loads every JPEG (sorted by name) and reads its size; broken files are skipped with a warning.
std::vector<JpegFile> load_jpegs(const fs::path& input) {
    std::vector<fs::path> paths;
    if (fs::is_directory(input)) {
        for (const auto& entry : fs::directory_iterator(input)) {
            if (entry.is_regular_file() && is_jpeg(entry.path())) {
                paths.push_back(entry.path());
            }
        }
        std::sort(paths.begin(), paths.end());
    } else if (fs::is_regular_file(input)) {
        paths.push_back(input);
    }

    std::vector<JpegFile> files;
    kvm::JpegDecoder decoder;
    kvm::BgraImage image;
    for (const auto& path : paths) {
        std::ifstream in(path, std::ios::binary);
        JpegFile file{.name = utf8(path.filename().wstring()),
                      .bytes = {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}};
        if (!decoder.decode(file.bytes, image)) {
            kvm::log::warn("skipping {}: {}", file.name, decoder.last_error());
            continue;
        }
        file.width = image.width;
        file.height = image.height;
        files.push_back(std::move(file));
    }
    return files;
}

// Sends frames to one client until it disconnects. Returns the number of frames sent.
std::uint64_t stream_to(kvm::net::FrameChannel& channel, const std::vector<JpegFile>& files, int fps,
                        std::uint32_t& seq) {
    const auto period = std::chrono::microseconds(1'000'000 / fps);
    auto next = Clock::now();
    auto stats_start = Clock::now();
    std::uint64_t sent = 0;
    std::uint64_t bytes_since_stats = 0;
    std::uint64_t frames_since_stats = 0;

    for (std::size_t i = 0;; i = (i + 1) % files.size()) {
        const JpegFile& file = files[i];
        const kvm::proto::FrameHeader header{.seq = seq++,
                                             .t_capture_us = kvm::steady_now_us(),
                                             .width = file.width,
                                             .height = file.height,
                                             .codec = "jpeg"};
        if (!channel.send_frame(header, file.bytes)) {
            return sent;
        }
        ++sent;
        ++frames_since_stats;
        bytes_since_stats += file.bytes.size();

        const auto now = Clock::now();
        if (now - stats_start >= kStatsInterval) {
            const double secs = std::chrono::duration<double>(now - stats_start).count();
            kvm::log::info("sent {} frames: {:.1f} fps, {:.1f} Mbit/s", sent, frames_since_stats / secs,
                           bytes_since_stats * 8 / secs / 1e6);
            stats_start = now;
            frames_since_stats = 0;
            bytes_since_stats = 0;
        }

        next += period;
        if (next < now) {
            next = now;  // fell behind (slow network): don't burst to catch up
        }
        std::this_thread::sleep_until(next);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        kvm::log::error("usage: fake_streamer.exe <folder | image.jpg> [--port N] [--fps N]");
        return 1;
    }

    try {
        const kvm::Config cfg = kvm::load_config();
        std::uint16_t port = cfg.video_port;
        int fps = cfg.max_fps;
        for (int i = 2; i + 1 < argc; ++i) {
            const std::wstring_view arg = argv[i];
            if (arg == L"--port") {
                port = static_cast<std::uint16_t>(_wtoi(argv[++i]));
            } else if (arg == L"--fps") {
                fps = std::clamp(_wtoi(argv[++i]), 1, 240);
            }
        }

        const fs::path input = argv[1];
        const auto files = load_jpegs(input);
        if (files.empty()) {
            kvm::log::error("no usable .jpg files at {}", utf8(input.wstring()));
            return 1;
        }
        kvm::log::info("loaded {} JPEG(s), first {} is {}x{}", files.size(), files[0].name, files[0].width,
                       files[0].height);

        kvm::net::VideoServer server(port);
        kvm::log::info("listening on port {} ({} fps); waiting for a viewer to connect...", port, fps);

        std::uint32_t seq = 0;
        while (auto channel = server.accept()) {
            kvm::log::info("viewer connected from {}", channel->peer());
            const auto sent = stream_to(*channel, files, fps, seq);
            kvm::log::info("viewer {} disconnected after {} frames; waiting for the next one", channel->peer(), sent);
        }
        return 0;
    } catch (const std::exception& e) {
        kvm::log::error("{}", e.what());
        return 1;
    }
}
