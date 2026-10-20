// SPDX-License-Identifier: Apache-2.0

// window_share_test: one WindowShare from start to end, with a fake channel instead of the network.
// The window must go onto the VDD and fill it, JPEG frames of the VDD size must come out, and after
// the share ends the window must be exactly where it was and the VDD detached. A share of a window
// that doesn't exist must fail cleanly.
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
    auto fake_send = [&](const kvm::proto::FrameHeader& h, std::span<const std::uint8_t> payload) {
        std::lock_guard lock(mutex);
        if (!payload.empty()) received.push_back(h);
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

    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
