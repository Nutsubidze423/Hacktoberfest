// SPDX-License-Identifier: Apache-2.0

// file_drop_test: the "drop here" catcher (T9).
//
//   file_drop_test.exe            automatic: shows, hides after its timeout, hide() works
//   file_drop_test.exe --manual   shows the box in the middle of the screen for 20 s: drag files from
//                                 Explorer onto it and the test prints their paths
//
// A real drag can't be automated from a background process (Windows only lets the foreground app
// capture the mouse during DoDragDrop), hence the manual mode.

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include "kvm/dpi.h"
#include "kvm/file_drop.h"
#include "kvm/log.h"

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

struct Checks {
    int failed = 0;
    void operator()(bool ok, std::string_view what) {
        kvm::log::info("{} {}", ok ? "[ok]  " : "[FAIL]", what);
        if (!ok) ++failed;
    }
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
    kvm::display::enable_dpi_awareness();
    const bool manual = argc > 1 && std::wstring_view(argv[1]) == L"--manual";
    Checks check;

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<fs::path> dropped;
    kvm::files::FileDropCatcher catcher(
        [&](std::vector<fs::path> paths) {
            {
                std::lock_guard lock(mutex);
                dropped = std::move(paths);
            }
            cv.notify_all();
        },
        manual ? L"Drop files here (test)" : L"Drop here to send (test)");

    const auto primary = kvm::display::primary_monitor().rect;
    const POINT center{(primary.left + primary.right) / 2, (primary.top + primary.bottom) / 2};

    if (!manual) {
        catcher.show_at(center, 300ms);
        std::this_thread::sleep_for(100ms);
        check(catcher.visible(), "shown at the screen center");
        std::this_thread::sleep_for(600ms);
        check(!catcher.visible(), "hides again after its timeout when nothing is dragged");
        catcher.show_at(POINT{primary.right - 1, center.y}, 10s);  // on the very edge, like D1 will
        std::this_thread::sleep_for(100ms);
        check(catcher.visible(), "shown at the right screen edge");
        catcher.hide();
        std::this_thread::sleep_for(100ms);
        check(!catcher.visible(), "hide() hides it");
    } else {
        catcher.show_at(center, 20s);
        kvm::log::info("drag one or more files from Explorer onto the box in the middle of the screen (20 s)...");
        std::unique_lock lock(mutex);
        cv.wait_for(lock, 20s, [&] { return !dropped.empty(); });
        for (const auto& p : dropped) kvm::log::info("  dropped: {}", p.string());
        check(!dropped.empty(), std::format("on_drop got {} path(s)", dropped.size()));
        lock.unlock();
        std::this_thread::sleep_for(200ms);
        check(!catcher.visible(), "the box hid after the drop");
    }

    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
