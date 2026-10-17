// viewer_test: run the B-side viewer without A.
//
//   viewer_test.exe <image.jpg | folder> [--fps 30]
//       Local playback: decodes the JPEG(s) (logs a decode benchmark) and plays them in a loop from
//       a background thread, like the real video receiver. No network.
//   viewer_test.exe --connect <host> [--port 50501]
//       Live stream over the network: does what main.cpp does for the video channel (connect,
//       reconnect every second, link state) and hands the channel to a VideoReceiver. Works with
//       tools/fake_streamer.exe (JPEG) and with kvm_host.exe --no-kvm --test-video (codec "test":
//       not shown, but counted in the HUD's MBPS).
//
// Check: the image looks right, keeps its aspect ratio when resized, and the HUD shows FPS etc.
// Messages that would go to the window owner (input, stream_stop) are logged as "to owner: {...}".

#include <algorithm>
#include <chrono>
#include <cstdint>
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
#include "kvm/net.h"
#include "display_settings.h"
#include "tray_icon.h"
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

constexpr auto kConnectTimeout = std::chrono::seconds(2);
constexpr auto kReconnectDelay = std::chrono::seconds(1);
constexpr auto kPollInterval = std::chrono::milliseconds(50);

// No control connection here: messages for the window owner are logged instead.
bool log_to_owner(const kvm::proto::Message& msg) {
    kvm::log::info("to owner: {}", kvm::proto::encode(msg).dump());
    return true;
}

void sleep_unless_stopped(std::stop_token& stop, std::chrono::milliseconds duration) {
    const auto until = Clock::now() + duration;
    while (!stop.stop_requested() && Clock::now() < until) {
        std::this_thread::sleep_for(kPollInterval);
    }
}

// Stands in for main.cpp's video side: it owns the channel, reconnects and sets the link state;
// the VideoReceiver only reads from the channel it is given.
void video_link_loop(std::stop_token stop, const std::string& host, std::uint16_t port, kvm::ViewerWindow& viewer) {
    const bool same_machine = host == "127.0.0.1" || host == "localhost";
    bool lost = false;  // after a drop, the viewer keeps showing LOST until the link is back
    while (!stop.stop_requested()) {
        if (!lost) {
            viewer.set_link_state(kvm::LinkState::Connecting);
        }
        auto channel = kvm::net::connect_video(host, port, kConnectTimeout);
        if (!channel) {
            if (!lost) {
                viewer.set_link_state(kvm::LinkState::Waiting);
            }
            sleep_unless_stopped(stop, kReconnectDelay);
            continue;
        }
        viewer.set_link_state(kvm::LinkState::Linked);
        lost = false;
        {
            kvm::VideoReceiver receiver(*channel, viewer);
            if (same_machine) {
                receiver.set_clock_offset_us(0);  // same machine, same clock: latency is exact
            }
            while (!stop.stop_requested() && receiver.running()) {
                std::this_thread::sleep_for(kPollInterval);
            }
        }  // the receiver closes the channel and joins before `channel` is destroyed
        if (!stop.stop_requested()) {
            kvm::log::warn("video: connection lost, reconnecting");
            viewer.set_link_state(kvm::LinkState::Lost);
            lost = true;
        }
    }
}

std::vector<uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void play_loop(std::stop_token stop, const std::vector<std::vector<uint8_t>>& jpegs, int fps, kvm::ViewerWindow& viewer) {
    kvm::JpegDecoder decoder;
    kvm::BgraImage spare;  // reused: present() hands back a spare buffer
    const auto period = std::chrono::microseconds(1'000'000 / fps);
    auto next = Clock::now();
    std::uint32_t seq = 0;

    for (std::size_t index = 0; !stop.stop_requested(); index = (index + 1) % jpegs.size()) {
        if (decoder.decode(jpegs[index], spare)) {
            viewer.count_payload_bytes(jpegs[index].size());
            // Decoded right now, so capture -> display is exact on this machine.
            viewer.present(spare, {.seq = seq++, .t_capture_local_us = kvm::steady_now_us()});
        } else {
            kvm::log::warn("frame {}: {}", index + 1, decoder.last_error());
        }
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
    if (!viewer.create()) {
        kvm::log::error("could not create the viewer window (error {})", GetLastError());
        return 1;
    }
    viewer.set_jpeg_quality(cfg.jpeg_quality);
    viewer.set_endpoint(host, port);
    viewer.set_reconnects(true);
    viewer.set_message_sink(log_to_owner);

    // Tray icon + "Connected to host" / "Connection lost" notifications.
    const std::wstring endpoint = std::format(L"{}:{}", argv[2], port);
    kvm::DisplaySettings displays;
    kvm::TrayIcon tray;
    tray.create(kvm::ui::kAppName, {.open = [&] { viewer.bring_to_front(); },
                                .displays = [&] { displays.show({.role = kvm::layout::Role::Client}); },
                                .quit = [&] { PostMessageW(viewer.hwnd(), WM_CLOSE, 0, 0); }});
    viewer.set_link_listener(
        [&](kvm::LinkState before, kvm::LinkState now) { tray.on_link_change(before, now, endpoint, true); });

    // Declared after viewer and tray: stopped and joined first.
    std::jthread video(video_link_loop, host, port, std::ref(viewer));
    pump_messages();
    return 0;
}

int run_local(const fs::path& input, int fps) {
    const auto files = collect_inputs(input);
    if (files.empty()) {
        kvm::log::error("no .jpg files at {}", utf8(input.wstring()));
        return 1;
    }
    std::vector<std::vector<uint8_t>> jpegs;
    for (const auto& f : files) {
        jpegs.push_back(read_file(f));
    }

    // Decode every file once to catch broken ones, then benchmark the first.
    kvm::JpegDecoder decoder;
    kvm::BgraImage first;
    for (std::size_t i = 0; i < jpegs.size(); ++i) {
        kvm::BgraImage image;
        if (!decoder.decode(jpegs[i], image)) {
            kvm::log::error("{}: {}", utf8(files[i].filename().wstring()), decoder.last_error());
            return 1;
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
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        best_ms = std::min(best_ms, ms);
        total_ms += ms;
    }
    kvm::log::info("{} file(s); {}: {}x{}, {} KB, decode avg {:.2f} ms / best {:.2f} ms ({} runs)", jpegs.size(),
                   utf8(files[0].filename().wstring()), first.width, first.height, jpegs[0].size() / 1024,
                   total_ms / kBenchmarkRuns, best_ms, kBenchmarkRuns);

    kvm::ViewerWindow viewer;
    if (!viewer.create()) {
        kvm::log::error("could not create the viewer window (error {})", GetLastError());
        return 1;
    }
    viewer.set_message_sink(log_to_owner);
    // Local playback stands in for a linked owner streaming the folder. hwnd 0 is a placeholder:
    // closing the window logs the stream_stop that would give the window back.
    viewer.set_link_state(kvm::LinkState::Linked);
    viewer.start_stream({.hwnd = 0, .title = utf8(input.filename().wstring()), .width = first.width,
                         .height = first.height});

    kvm::log::info("playing at {} fps; close the window to quit", fps);
    std::jthread player(play_loop, std::cref(jpegs), fps, std::ref(viewer));  // stops before viewer dies
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
    int fps = 30;
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--fps") {
            fps = std::clamp(_wtoi(argv[++i]), 1, 240);
        }
    }
    return run_local(argv[1], fps);
}
