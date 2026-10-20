// SPDX-License-Identifier: Apache-2.0

// window_mover_test: sends one window to the VDD and back, and checks that it fills the VDD and
// returns exactly where it was. The window is always restored and the VDD detached at the end.
//
//   window_mover_test.exe                  pick the window from a list
//   window_mover_test.exe --title Notepad  first window whose title contains "Notepad"

#include <chrono>
#include <cwchar>
#include <format>
#include <string>
#include <string_view>
#include <thread>

#include "kvm/dpi.h"
#include "kvm/log.h"
#include "virtual_display.h"
#include "window_mover.h"
#include "window_picker.h"

namespace {

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

std::string rect_text(const RECT& r) {
    return std::format("({}, {}) {}x{}", r.left, r.top, r.right - r.left, r.bottom - r.top);
}

bool same_placement(const WINDOWPLACEMENT& a, const WINDOWPLACEMENT& b) {
    return a.showCmd == b.showCmd && EqualRect(&a.rcNormalPosition, &b.rcNormalPosition);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    kvm::display::enable_dpi_awareness();

    std::optional<kvm::WindowInfo> window;
    if (argc >= 3 && std::wstring_view(argv[1]) == L"--title") {
        window = window_with_title(argv[2]);
        if (!window) {
            kvm::log::error("no window with \"{}\" in its title", kvm::to_utf8(argv[2]));
            return 1;
        }
    } else {
        window = kvm::pick_window_console();
        if (!window) return 0;
    }
    const HWND hwnd = window->hwnd;
    kvm::log::info("window: \"{}\" ({})", kvm::to_utf8(window->title), kvm::to_utf8(window->process_name));

    auto vdd = kvm::VirtualDisplay::find();
    if (!vdd) {
        kvm::log::error("Virtual Display Driver not found");
        return 1;
    }

    WINDOWPLACEMENT before{};
    before.length = sizeof(before);
    GetWindowPlacement(hwnd, &before);
    RECT before_rect{};
    GetWindowRect(hwnd, &before_rect);
    kvm::log::info("before: {} (show state {})", rect_text(before_rect), before.showCmd);

    kvm::WindowMover mover;
    // Cleanup runs on every exit path: window back first, then the VDD off.
    struct Cleanup {
        kvm::WindowMover& mover;
        kvm::VirtualDisplay& vdd;
        HWND hwnd;
        ~Cleanup() {
            if (mover.sent_window()) mover.restore(hwnd);
            vdd.detach();
        }
    } cleanup{mover, *vdd, hwnd};

    Checks check;
    check(vdd->attach(1920, 1080, 60, kvm::display::Side::Right), "VDD attached, 1920x1080 on the right");
    const auto target = vdd->rect();
    if (!target) return 1;

    const auto result = mover.send(hwnd, target->rect);
    check(result == kvm::WindowMover::Result::Ok, "send to the VDD");
    if (result == kvm::WindowMover::Result::Ok) {
        RECT on_vdd{};
        GetWindowRect(hwnd, &on_vdd);
        RECT client{};
        GetClientRect(hwnd, &client);
        check(IsZoomed(hwnd) != FALSE, "maximized");
        check(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) == target->monitor, "on the VDD monitor");
        kvm::log::info("on the VDD: window {}, client {}x{}, VDD {}", rect_text(on_vdd), client.right, client.bottom,
                       rect_text(target->rect));
        std::this_thread::sleep_for(std::chrono::seconds(2));  // time to see that it left A's screen

        check(mover.restore(hwnd), "restore");
        WINDOWPLACEMENT after{};
        after.length = sizeof(after);
        GetWindowPlacement(hwnd, &after);
        RECT after_rect{};
        GetWindowRect(hwnd, &after_rect);
        kvm::log::info("after: {} (show state {})", rect_text(after_rect), after.showCmd);
        check(same_placement(before, after), "same placement as before");
        check(EqualRect(&before_rect, &after_rect) != FALSE, "same window rect as before");
    }

    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
