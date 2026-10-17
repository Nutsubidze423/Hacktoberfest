// desk_test: desk mode on one PC. The VDD is attached next to the primary monitor, and the windows
// dragged onto it are drawn back on the primary monitor by a DeskView, exactly as the other PC would
// draw them (the network is a loopback lambda). Drag a window over the edge and its "ghost" appears
// on the opposite side of this screen.
//
//   desk_test.exe [--side right|left] [--seconds N] [--list]   watch it by hand (Ctrl+C ends)
//   desk_test.exe --auto [--title Paint]                       automatic checks with an open Paint window
//
// --auto moves the window half onto the VDD, checks the tracked rect, that frames go out and get
// drawn, that the overlay hides once the window is back, then puts the window back and detaches.

#include <windows.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include "desk_share.h"
#include "desk_view.h"
#include "kvm/desk_region.h"
#include "kvm/dpi.h"
#include "kvm/log.h"
#include "virtual_display.h"
#include "window_picker.h"

namespace {

using namespace std::chrono_literals;

std::atomic<bool> g_stop{false};

BOOL WINAPI on_ctrl(DWORD) {
    g_stop = true;
    return TRUE;
}

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

template <class Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred();
}

void print_windows(const kvm::DeskShare& share) {
    for (const auto& w : share.tracker().windows()) {
        kvm::log::info("  {} \"{}\" [{}] {},{} {}x{}", reinterpret_cast<void*>(w.hwnd), w.title, w.cls, w.rect.left,
                       w.rect.top, w.rect.width(), w.rect.height());
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    kvm::display::enable_dpi_awareness();
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    auto side = kvm::display::Side::Right;
    int seconds = 0;
    bool list = false;
    bool automatic = false;
    std::wstring title = L"Paint";
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view a = argv[i];
        if (a == L"--side" && i + 1 < argc) {
            side = std::wstring_view(argv[++i]) == L"left" ? kvm::display::Side::Left : kvm::display::Side::Right;
        } else if (a == L"--seconds" && i + 1 < argc) {
            seconds = std::stoi(argv[++i]);
        } else if (a == L"--list") {
            list = true;
        } else if (a == L"--auto") {
            automatic = true;
        } else if (a == L"--title" && i + 1 < argc) {
            title = argv[++i];
        }
    }

    auto vdd = kvm::VirtualDisplay::find();
    if (!vdd) {
        kvm::log::error("the Virtual Display Driver is not installed");
        return 1;
    }
    const auto primary = kvm::display::primary_monitor();
    if (!vdd->attach(primary.rect.width(), primary.rect.height(), 60, side)) {
        kvm::log::error("could not attach the VDD");
        return 1;
    }
    const auto vdd_rect = vdd->rect();
    if (!vdd_rect) {
        kvm::log::error("the VDD is attached but has no monitor");
        vdd->detach();
        return 1;
    }
    const kvm::display::Rect vr{vdd_rect->rect.left, vdd_rect->rect.top, vdd_rect->rect.right, vdd_rect->rect.bottom};
    kvm::log::info("VDD {} at {},{} {}x{}", kvm::to_utf8(vdd->device()), vr.left, vr.top, vr.width(), vr.height());
    // The "other PC" is this screen: give the VDD its scale, as kvm_host does with the peer's.
    vdd->set_scale(primary.scale);

    Checks check;
    {
        kvm::desk::Region region;
        region.set_vdd({.device = kvm::to_utf8(vdd->device()), .rect = vr, .live = true});
        kvm::DeskView view;
        std::atomic<std::uint64_t> bytes{0};
        auto loopback = [&](const kvm::proto::FrameHeader& h, std::span<const std::uint8_t> payload) {
            bytes += payload.size();
            view.present(kvm::net::Frame{.header = h, .payload = {payload.begin(), payload.end()}});
            return true;
        };
        kvm::DeskShare share(*vdd_rect, region, loopback, {.jpeg_quality = 75, .max_fps = 30});

        if (automatic) {
            const auto window = window_with_title(title);
            check(window.has_value(), "a window to test with is open");
            if (window) {
                const HWND hwnd = window->hwnd;
                WINDOWPLACEMENT saved{sizeof(saved)};
                GetWindowPlacement(hwnd, &saved);
                ShowWindow(hwnd, SW_RESTORE);
                const int w = 800, h = 600;
                const int x = side == kvm::display::Side::Right ? vr.left - w / 2 : vr.right - w / 2;
                SetWindowPos(hwnd, HWND_TOP, x, vr.top + 100, w, h, SWP_NOACTIVATE);

                // The part on the VDD starts at the VDD's inner edge. (Its size changes: crossing onto a
                // monitor with another DPI scale rescales the window, like between real monitors.)
                const bool tracked = wait_until(
                    [&] {
                        for (const auto& r : region.rects()) {
                            const bool at_edge = side == kvm::display::Side::Right ? r.left == vr.left : r.right == vr.right;
                            if (at_edge && r.width() > 100 && r.width() < vr.width() && r.height() > 300) return true;
                        }
                        return false;
                    },
                    2s);
                check(tracked, "the half on the VDD is tracked");
                print_windows(share);
                check(wait_until([&] { return share.frames_sent() > 0; }, 3s), "frames go out");
                check(wait_until([&] { return view.frames_shown() > 0; }, 3s), "frames are drawn");
                check(wait_until([&] { return IsWindowVisible(view.hwnd()) != FALSE; }, 2s), "the overlay is shown");
                kvm::display::Point inside{side == kvm::display::Side::Right ? vr.left + 50 : vr.right - 50, vr.top + 300};
                check(region.hit(inside), "a point on the window hits");
                check(!region.hit({inside.x, vr.top + 900}), "a point below it doesn't");
                std::this_thread::sleep_for(1500ms);  // look at it: the ghost is on the other side of the screen

                SetWindowPlacement(hwnd, &saved);
                check(wait_until([&] { return region.rects().empty(); }, 2s), "back home: nothing tracked");
                check(wait_until([&] { return IsWindowVisible(view.hwnd()) == FALSE; }, 2s), "the overlay is hidden");
            }
        } else {
            kvm::log::info("drag a window over the {} edge; Ctrl+C ends", kvm::display::to_string(side));
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            while (!g_stop && (seconds == 0 || std::chrono::steady_clock::now() < end)) {
                std::this_thread::sleep_for(1s);
                kvm::log::info("sent {} frames ({} KB), drawn {}", share.frames_sent(), bytes / 1024,
                               view.frames_shown());
                if (list) print_windows(share);
            }
        }
    }
    vdd->detach();
    if (automatic && check.failed == 0) kvm::log::info("desk_test: all checks passed");
    if (automatic && check.failed != 0) kvm::log::error("desk_test: {} check(s) FAILED", check.failed);
    return check.failed == 0 ? 0 : 1;
}
