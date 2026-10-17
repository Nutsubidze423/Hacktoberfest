// viewer_test: show JPEGs in the B-side viewer window, no network needed.
//
//   viewer_test.exe <image.jpg>               decode once (+ timing benchmark) and show it
//   viewer_test.exe <folder> [--fps 30]       play every *.jpg in the folder in a loop from a
//                                             background thread, like the real video receiver
//   viewer_test.exe --connect <host> [--port 50501]
//                                             live stream over the network via VideoReceiver
//                                             (from tools/fake_streamer.exe, later from A)
//
// Check: the image looks right, keeps its aspect ratio when the window is resized, and the
// overlay shows size, FPS and decode time.

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "jpeg_decoder.h"
#include "kvm/config.h"
#include "kvm/log.h"
#include "video_receiver.h"
#include "viewer_window.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kBenchmarkRuns = 50;

std::string utf8(const std::wstring& s) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

bool is_jpeg(const fs::path& p) {
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return ext == L".jpg" || ext == L".jpeg";
}

std::vector<fs::path> collect_inputs(const fs::path& input) {
    std::vector<fs::path> files;
    if (fs::is_directory(input)) {
        for (const auto& entry : fs::directory_iterator(input)) {
            if (entry.is_regular_file() && is_jpeg(entry.path())) {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
    } else if (fs::is_regular_file(input)) {
        files.push_back(input);
    }
    return files;
}

std::vector<uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Initial client size: the image size, shrunk to fit 80% of the primary work area.
SIZE initial_client_size(int image_w, int image_h) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const RECT limit{0, 0, (work.right - work.left) * 8 / 10, (work.bottom - work.top) * 8 / 10};
    if (image_w <= limit.right && image_h <= limit.bottom) {
        return {image_w, image_h};
    }
    const RECT fit = kvm::fit_rect(image_w, image_h, limit);
    return {fit.right - fit.left, fit.bottom - fit.top};
}

void play_loop(std::stop_token stop, const std::vector<std::vector<uint8_t>>& jpegs, int fps,
               kvm::ViewerWindow& viewer) {
    kvm::JpegDecoder decoder;
    kvm::BgraImage spare;  // reused: present() hands back the previous frame's buffer
    const auto period = std::chrono::microseconds(1'000'000 / fps);
    auto next = Clock::now();
    size_t index = 0;

    while (!stop.stop_requested()) {
        const auto t0 = Clock::now();
        if (decoder.decode(jpegs[index], spare)) {
            const double decode_ms = ms_since(t0);
            viewer.present(spare);
            viewer.set_status(std::format(L"decode {:.2f} ms  frame {}/{}", decode_ms, index + 1, jpegs.size()));
        } else {
            kvm::log::warn("frame {}: {}", index + 1, decoder.last_error());
        }
        index = (index + 1) % jpegs.size();
        next += period;
        std::this_thread::sleep_until(next);
    }
}

void pump_messages() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

int run_connect(int argc, wchar_t** argv) {
    const kvm::Config cfg = kvm::load_config();
    const std::string host = utf8(argv[2]);
    std::uint16_t port = cfg.video_port;
    for (int i = 3; i + 1 < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--port") {
            port = static_cast<std::uint16_t>(_wtoi(argv[++i]));
        }
    }

    kvm::ViewerWindow viewer;
    if (!viewer.create(std::format(L"KVM viewer test - {}:{}", argv[2], port), 1280, 720)) {
        kvm::log::error("could not create the viewer window (error {})", GetLastError());
        return 1;
    }
    kvm::VideoReceiver receiver(host, port, viewer);  // declared after viewer: its threads stop first
    pump_messages();
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (argc < 2) {
        kvm::log::error("usage: viewer_test.exe <image.jpg | folder> [--fps N]  |  --connect <host> [--port N]");
        return 1;
    }
    if (std::wstring_view(argv[1]) == L"--connect") {
        if (argc < 3) {
            kvm::log::error("usage: viewer_test.exe --connect <host> [--port N]");
            return 1;
        }
        return run_connect(argc, argv);
    }
    const fs::path input = argv[1];
    int fps = 30;
    for (int i = 2; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--fps" && i + 1 < argc) {
            fps = std::clamp(_wtoi(argv[++i]), 1, 240);
        }
    }

    const auto files = collect_inputs(input);
    if (files.empty()) {
        kvm::log::error("no .jpg files at {}", utf8(input.wstring()));
        return 1;
    }

    // Load everything up front so the benchmark and playback measure decoding only, not disk I/O.
    std::vector<std::vector<uint8_t>> jpegs;
    for (const auto& f : files) {
        jpegs.push_back(read_file(f));
    }
    kvm::log::info("loaded {} file(s) from {}", jpegs.size(), utf8(input.wstring()));

    // Decode every file once to catch broken ones, then benchmark the first.
    kvm::JpegDecoder decoder;
    kvm::BgraImage first;
    for (size_t i = 0; i < jpegs.size(); ++i) {
        kvm::BgraImage image;
        if (!decoder.decode(jpegs[i], image)) {
            kvm::log::error("{}: {}", utf8(files[i].filename().wstring()), decoder.last_error());
            return 1;
        }
        if (!decoder.last_error().empty()) {
            kvm::log::warn("{}: {}", utf8(files[i].filename().wstring()), decoder.last_error());
        }
        if (i == 0) {
            first = std::move(image);
        }
    }

    double best_ms = 1e9;
    double total_ms = 0;
    kvm::BgraImage scratch;
    for (int i = 0; i < kBenchmarkRuns; ++i) {
        const auto t0 = Clock::now();
        decoder.decode(jpegs[0], scratch);
        const double ms = ms_since(t0);
        best_ms = std::min(best_ms, ms);
        total_ms += ms;
    }
    const double avg_ms = total_ms / kBenchmarkRuns;
    kvm::log::info("{}: {}x{}, {} KB, decode avg {:.2f} ms / best {:.2f} ms ({} runs)",
                   utf8(files[0].filename().wstring()), first.width, first.height, jpegs[0].size() / 1024, avg_ms,
                   best_ms, kBenchmarkRuns);

    const SIZE size = initial_client_size(first.width, first.height);
    kvm::ViewerWindow viewer;
    if (!viewer.create(L"KVM viewer test", size.cx, size.cy)) {
        kvm::log::error("could not create the viewer window (error {})", GetLastError());
        return 1;
    }

    std::jthread player;  // declared after viewer: stopped and joined before the window is destroyed
    if (jpegs.size() == 1) {
        viewer.present(first);
        viewer.set_status(std::format(L"decode avg {:.2f} ms / best {:.2f} ms", avg_ms, best_ms));
    } else {
        kvm::log::info("playing {} frames at {} fps; close the window to quit", jpegs.size(), fps);
        player = std::jthread(play_loop, std::cref(jpegs), fps, std::ref(viewer));
    }

    pump_messages();
    return 0;
}
