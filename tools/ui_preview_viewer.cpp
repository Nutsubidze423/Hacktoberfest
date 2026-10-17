// ui_preview_viewer: the viewer window in every state with FAKE data, no network
// (docs/DESIGN.md section 7). For design review only — none of these values are real.
//
//   ui_preview_viewer.exe                    cycle WAITING -> CONNECTING -> LINKED -> STREAM -> STALLED -> LOST
//   ui_preview_viewer.exe --state <name>     hold one: waiting | connecting | linked | streaming | stalled | lost
//   ui_preview_viewer.exe --image <file.jpg> stream this JPEG instead of the generated test pattern
//   ui_preview_viewer.exe --seconds <n>      time per state when cycling (default 4)

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>

#include "jpeg_decoder.h"
#include "kvm/log.h"
#include "kvm/net.h"
#include "viewer_window.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

enum class Phase { Waiting, Connecting, Linked, Streaming, Stalled, Lost };
constexpr std::array kCycle = {Phase::Waiting, Phase::Connecting, Phase::Linked,
                               Phase::Streaming, Phase::Stalled,    Phase::Lost};

// Fake values, chosen to exercise the UI (long Georgian title -> font fallback + ellipsis).
constexpr char kFakeHost[] = "192.168.1.10";
constexpr std::uint16_t kFakePort = 50501;
constexpr int kFakeRttMs = 3;
constexpr int kFakeQuality = 75;
constexpr auto kFakeLatency = 38ms;
constexpr std::size_t kFakePayloadBytes = 140 * 1024;
constexpr std::uint32_t kFakeDropEvery = 45;  // skip one seq now and then so DROP is not 0
constexpr int kFps = 30;
constexpr wchar_t kFakeTitle[] =
    L"ფოტოშოპი — Untitled-1.psd @ 66.7% (Layer 1, RGB/8) · Adobe Photoshop 2025";

std::optional<Phase> parse_phase(std::wstring_view s) {
    if (s == L"waiting") return Phase::Waiting;
    if (s == L"connecting") return Phase::Connecting;
    if (s == L"linked") return Phase::Linked;
    if (s == L"streaming") return Phase::Streaming;
    if (s == L"stalled") return Phase::Stalled;
    if (s == L"lost") return Phase::Lost;
    return std::nullopt;
}

// 1920x1080 test pattern: gradient, 120 px grid, colored corners (orientation/color check).
kvm::BgraImage make_pattern() {
    kvm::BgraImage img;
    img.width = 1920;
    img.height = 1080;
    img.pixels.resize(img.stride() * img.height);
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            std::uint8_t* px = &img.pixels[(static_cast<std::size_t>(y) * img.width + x) * 4];
            const bool grid = x % 120 == 0 || y % 120 == 0;
            std::uint8_t r = static_cast<std::uint8_t>(30 + 50 * x / img.width);
            std::uint8_t g = static_cast<std::uint8_t>(32 + 10 * y / img.height);
            std::uint8_t b = static_cast<std::uint8_t>(60 + 30 * y / img.height);
            if (grid) r = g = b = 90;
            const int c = 160;
            if (x < c && y < c) { r = 255; g = 0; b = 0; }                                    // top-left red
            if (x >= img.width - c && y < c) { r = 0; g = 255; b = 0; }                       // top-right green
            if (x < c && y >= img.height - c) { r = 0; g = 0; b = 255; }                      // bottom-left blue
            if (x >= img.width - c && y >= img.height - c) { r = 255; g = 255; b = 255; }     // bottom-right white
            px[0] = b;
            px[1] = g;
            px[2] = r;
            px[3] = 255;
        }
    }
    return img;
}

void draw_marker(kvm::BgraImage& img, int frame) {
    const int size = 120;
    const int x0 = (frame * 12) % (img.width - size);
    const int y0 = (img.height - size) / 2;
    for (int y = y0; y < y0 + size; ++y) {
        for (int x = x0; x < x0 + size; ++x) {
            std::uint8_t* px = &img.pixels[(static_cast<std::size_t>(y) * img.width + x) * 4];
            px[0] = 0x1F;
            px[1] = 0x5A;
            px[2] = 0xFF;
        }
    }
}

class Driver {
public:
    Driver(kvm::ViewerWindow& viewer, kvm::BgraImage base) : viewer_(viewer), base_(std::move(base)) {}

    void run(std::stop_token stop, std::optional<Phase> hold, std::chrono::milliseconds per_phase) {
        viewer_.set_endpoint(kFakeHost, kFakePort);
        viewer_.set_reconnects(true);
        viewer_.set_jpeg_quality(kFakeQuality);
        for (std::size_t i = 0; !stop.stop_requested(); ++i) {
            const Phase phase = hold ? *hold : kCycle[i % kCycle.size()];
            enter(phase);
            const auto until = Clock::now() + (hold ? std::chrono::hours(24 * 365) : per_phase);
            if (phase == Phase::Streaming || phase == Phase::Stalled) {
                stream_until(stop, phase == Phase::Stalled ? Clock::now() + 300ms : until);
            }
            while (!stop.stop_requested() && Clock::now() < until) {
                std::this_thread::sleep_for(50ms);
            }
        }
    }

private:
    void enter(Phase phase) {
        switch (phase) {
        case Phase::Waiting:
            viewer_.set_rtt_ms(std::nullopt);
            viewer_.set_link_state(kvm::LinkState::Waiting);
            break;
        case Phase::Connecting:
            viewer_.set_link_state(kvm::LinkState::Connecting);
            break;
        case Phase::Linked:
            viewer_.set_link_state(kvm::LinkState::Linked);
            viewer_.set_rtt_ms(kFakeRttMs);
            viewer_.stop_stream();
            break;
        case Phase::Streaming:
            viewer_.set_link_state(kvm::LinkState::Linked);
            viewer_.set_rtt_ms(kFakeRttMs);
            if (!streaming_) {
                viewer_.start_stream(kFakeTitle, base_.width, base_.height);
                streaming_ = true;
            }
            break;
        case Phase::Stalled:
            // Same stream, but frames stop after a moment: the viewer turns it into STALLED.
            viewer_.set_link_state(kvm::LinkState::Linked);
            viewer_.set_rtt_ms(kFakeRttMs);
            if (!streaming_) {
                viewer_.start_stream(kFakeTitle, base_.width, base_.height);
                streaming_ = true;
            }
            break;
        case Phase::Lost:
            viewer_.set_link_state(kvm::LinkState::Lost);
            streaming_ = false;
            break;
        }
    }

    void stream_until(std::stop_token& stop, Clock::time_point until) {
        kvm::BgraImage frame;
        auto next = Clock::now();
        while (!stop.stop_requested() && Clock::now() < until) {
            frame.width = base_.width;
            frame.height = base_.height;
            frame.pixels.resize(base_.pixels.size());
            std::memcpy(frame.pixels.data(), base_.pixels.data(), base_.pixels.size());
            draw_marker(frame, frame_++);

            seq_ += seq_ % kFakeDropEvery == 0 ? 2 : 1;
            const auto capture = kvm::steady_now_us() - std::chrono::duration_cast<std::chrono::microseconds>(kFakeLatency).count();
            viewer_.count_payload_bytes(kFakePayloadBytes);
            viewer_.present(frame, {.seq = seq_, .t_capture_local_us = capture});

            next += std::chrono::microseconds(1'000'000 / kFps);
            std::this_thread::sleep_until(next);
        }
    }

    kvm::ViewerWindow& viewer_;
    kvm::BgraImage base_;
    bool streaming_ = false;
    int frame_ = 0;
    std::uint32_t seq_ = 0;
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::optional<Phase> hold;
    std::optional<std::filesystem::path> image_path;
    int seconds = 4;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == L"--state") {
            hold = parse_phase(argv[++i]);
            if (!hold) {
                kvm::log::error("unknown state; use waiting|connecting|linked|streaming|stalled|lost");
                return 1;
            }
        } else if (arg == L"--image") {
            image_path = argv[++i];
        } else if (arg == L"--seconds") {
            seconds = std::max(1, _wtoi(argv[++i]));
        }
    }

    kvm::BgraImage base;
    if (image_path) {
        std::ifstream in(*image_path, std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        kvm::JpegDecoder decoder;
        if (!decoder.decode(bytes, base)) {
            kvm::log::error("cannot decode image: {}", decoder.last_error());
            return 1;
        }
    } else {
        base = make_pattern();
    }

    kvm::ViewerWindow viewer;
    if (!viewer.create()) {
        kvm::log::error("could not create the viewer window (error {})", GetLastError());
        return 1;
    }
    kvm::log::info("preview: FAKE data; {}", hold ? "holding one state" : "cycling all states");

    Driver driver(viewer, std::move(base));
    std::jthread thread([&](std::stop_token stop) { driver.run(stop, hold, std::chrono::seconds(seconds)); });

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
