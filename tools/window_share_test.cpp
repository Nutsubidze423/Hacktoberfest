// SPDX-License-Identifier: Apache-2.0

// window_share_test: one WindowShare from start to end, with a fake channel instead of the network.
// The window must go onto the VDD and fill it, JPEG frames of the VDD size must come out, and after
// the share ends the window must be exactly where it was and the VDD detached. A share of a window
// that doesn't exist must fail cleanly.
// Then the same with --codec h264 (roadmap P2.4c), where this PC has a hardware encoder: every frame
// passes the viewer's framing check, the first is a keyframe, seq has no gaps, frames keep coming
// after the cursor stops, and every frame decodes to 1920x1080. (The 500 ms repeat of a still
// picture is h264_test's: here WGC keeps delivering frames.)
//
//   window_share_test.exe --title Paint

#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "h264_decoder.h"
#include "kvm/annexb.h"
#include "kvm/dpi.h"
#include "kvm/log.h"
#include "window_picker.h"
#include "window_share.h"

namespace {

using namespace std::chrono_literals;

struct Checks {
    int failed = 0;
    void operator()(bool ok, std::string_view what) {
        kvm::log::info("{} {}", ok ? "[ok]  " : "[FAIL]", what);
        if (!ok) ++failed;
    }
};

std::optional<kvm::WindowInfo> window_with_title(std::wstring_view part) {
    for (auto& w : kvm::list_capturable_windows()) {
        if (w.title.find(part) != std::wstring::npos) return w;
    }
    return std::nullopt;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    kvm::display::enable_dpi_awareness();
    const std::wstring title = argc >= 3 && std::wstring_view(argv[1]) == L"--title" ? argv[2] : L"Paint";
    const auto window = window_with_title(title);
    if (!window) {
        kvm::log::error("no window with \"{}\" in its title", kvm::to_utf8(title));
        return 1;
    }
    const HWND hwnd = window->hwnd;
    RECT before{};
    GetWindowRect(hwnd, &before);
    Checks check;

    std::mutex mutex;
    std::vector<kvm::proto::FrameHeader> received;
    std::vector<std::vector<std::uint8_t>> payloads;  // kept for h264, to decode
    auto fake_send = [&](const kvm::proto::FrameHeader& h, std::span<const std::uint8_t> payload) {
        std::lock_guard lock(mutex);
        if (!payload.empty()) received.push_back(h);
        if (h.codec == "h264") payloads.emplace_back(payload.begin(), payload.end());
        return true;
    };

    {
        std::unique_ptr<kvm::WindowShare> share;
        try {
            share = kvm::WindowShare::start(hwnd, fake_send, {});
        } catch (const std::exception& e) {
            kvm::log::error("{}", e.what());
            return 1;
        }
        check(share->width() == 1920 && share->height() == 1080, "stream is 1920x1080 (the VDD mode)");
        check(share->alive(), "share is alive");
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL), &mi);
        check(EqualRect(&mi.rcMonitor, &share->monitor_rect()) && IsZoomed(hwnd), "window is maximized on the VDD");

        // Move the cursor over the VDD for a moment so WGC has changes to deliver.
        POINT cursor{};
        GetCursorPos(&cursor);
        const RECT& r = share->monitor_rect();
        for (int i = 0; i < 60; ++i) {
            SetCursorPos((r.left + r.right) / 2 + static_cast<int>(80 * std::cos(i * 0.3)),
                         (r.top + r.bottom) / 2 + static_cast<int>(80 * std::sin(i * 0.3)));
            std::this_thread::sleep_for(16ms);
        }
        SetCursorPos(cursor.x, cursor.y);
        std::this_thread::sleep_for(200ms);

        std::lock_guard lock(mutex);
        check(received.size() >= 5, std::format("{} JPEG frames sent", received.size()));
        check(!received.empty() && received.back().width == 1920 && received.back().height == 1080 &&
                  received.back().codec == "jpeg",
              "frames are 1920x1080 jpeg");
    }  // share ends here

    RECT after{};
    GetWindowRect(hwnd, &after);
    check(EqualRect(&before, &after) != FALSE, "after the share the window is exactly where it was");
    const auto vdd = kvm::VirtualDisplay::find();
    check(vdd && !vdd->attached(), "VDD detached");

    bool threw = false;
    try {
        kvm::WindowShare::start(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(0x1234)), fake_send, {});
    } catch (const std::exception& e) {
        threw = true;
        kvm::log::info("expected error: {}", e.what());
    }
    check(threw && vdd && !vdd->attached(), "sharing a window that doesn't exist fails and leaves the VDD off");

    // --codec h264: the same share, encoded on the GPU.
    {
        std::lock_guard lock(mutex);
        received.clear();
    }
    bool h264 = false;
    {
        std::unique_ptr<kvm::WindowShare> share;
        try {
            share = kvm::WindowShare::start(hwnd, fake_send, {.codec = "h264"});
        } catch (const std::exception& e) {
            kvm::log::error("{}", e.what());
            return 1;
        }
        h264 = std::string_view(share->codec()) == "h264";
        if (!h264) kvm::log::info("       no hardware H.264 encoder here: the share fell back to JPEG");
        POINT cursor{};
        GetCursorPos(&cursor);
        const RECT& r = share->monitor_rect();
        for (int i = 0; i < 60; ++i) {
            SetCursorPos((r.left + r.right) / 2 + static_cast<int>(80 * std::cos(i * 0.3)),
                         (r.top + r.bottom) / 2 + static_cast<int>(80 * std::sin(i * 0.3)));
            std::this_thread::sleep_for(16ms);
        }
        SetCursorPos(cursor.x, cursor.y);
        std::size_t moving = 0;
        {
            std::this_thread::sleep_for(200ms);
            std::lock_guard lock(mutex);
            moving = received.size();
        }
        std::this_thread::sleep_for(1300ms);
        std::lock_guard lock(mutex);
        if (h264) {
            check(received.size() >= moving + 2,
                  std::format("{} h264 frames while the cursor moved, {} more after it stopped", moving,
                              received.size() - moving));
        }
    }
    if (h264) {
        std::lock_guard lock(mutex);
        bool in_order = !received.empty() && received.front().keyframe;
        for (std::size_t i = 1; i < received.size(); ++i)
            in_order = in_order && received[i].seq == received[i - 1].seq + 1;
        check(in_order, "the first h264 frame is a keyframe, and seq has no gaps");
        std::size_t framed = 0;
        for (std::size_t i = 0; i < payloads.size(); ++i) {
            if (kvm::annexb::check(payloads[i], received[i].keyframe).empty()) ++framed;
        }
        check(framed == payloads.size() && framed == received.size(),
              std::format("{} of {} pass the viewer's framing check", framed, received.size()));
        std::string why;
        auto decoder = kvm::H264Decoder::create(nullptr, why);
        std::size_t decoded = 0;
        for (std::size_t i = 0; decoder && i < payloads.size(); ++i) {
            const auto picture = decoder->decode(payloads[i], received[i].t_capture_us);
            if (picture && picture->width == 1920 && picture->height == 1080) ++decoded;
        }
        check(decoded == payloads.size(), std::format("{} of {} decode to 1920x1080", decoded, payloads.size()));
    }
    GetWindowRect(hwnd, &after);
    check(EqualRect(&before, &after) != FALSE, "after the h264 share too, the window is exactly where it was");

    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
