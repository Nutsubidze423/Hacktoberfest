// remote_input_test: the AnyDesk-like loop without a viewer. Paint is shared onto the VDD and the
// test sends exactly what a viewer would: remote_mouse_move / mouse_button to draw a line, then
// key messages for Ctrl+Z. The streamed JPEGs must show the line appear and disappear again.
// Also checks that Windows keys are refused and release_all() frees held keys.
// Use a fresh, empty Paint window; the test draws on its canvas.
//
//   remote_input_test.exe [--title Paint]

#include <turbojpeg.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "kvm/dpi.h"
#include "kvm/log.h"
#include "remote_input.h"
#include "window_picker.h"
#include "window_share.h"

namespace {

using namespace std::chrono_literals;
namespace proto = kvm::proto;

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

// The newest streamed JPEG, as the viewer would receive it.
struct LatestJpeg {
    std::mutex mutex;
    std::vector<std::uint8_t> bytes;
    std::uint64_t count = 0;
};

// Dark pixels on a horizontal band of the decoded frame (rows y-3..y+3, columns x0..x1),
// counting each column once. A drawn black line makes most columns dark.
int dark_columns(LatestJpeg& latest, double y_norm, double x0_norm, double x1_norm) {
    std::vector<std::uint8_t> jpeg;
    {
        std::lock_guard lock(latest.mutex);
        jpeg = latest.bytes;
    }
    tjhandle h = tj3Init(TJINIT_DECOMPRESS);
    int dark = -1;
    if (h && tj3DecompressHeader(h, jpeg.data(), jpeg.size()) == 0) {
        const int w = tj3Get(h, TJPARAM_JPEGWIDTH), ht = tj3Get(h, TJPARAM_JPEGHEIGHT);
        std::vector<std::uint8_t> bgra(static_cast<size_t>(w) * ht * 4);
        if (tj3Decompress8(h, jpeg.data(), jpeg.size(), bgra.data(), w * 4, TJPF_BGRX) == 0) {
            dark = 0;
            const int y = static_cast<int>(y_norm * (ht - 1));
            for (int x = static_cast<int>(x0_norm * (w - 1)); x <= static_cast<int>(x1_norm * (w - 1)); ++x) {
                for (int dy = -3; dy <= 3; ++dy) {
                    const auto* p = bgra.data() + (static_cast<size_t>(y + dy) * w + x) * 4;
                    if (p[0] + p[1] + p[2] < 3 * 90) {
                        ++dark;
                        break;
                    }
                }
            }
        }
    }
    tj3Destroy(h);
    return dark;
}

// Gives the app time to repaint and WGC time to deliver the new frame.
void settle() {
    std::this_thread::sleep_for(500ms);
}

// Keeps the frame of each step in captures/ to look at when a check fails.
void save(LatestJpeg& latest, const char* name) {
    std::lock_guard lock(latest.mutex);
    std::filesystem::create_directories("captures");
    std::ofstream(std::format("captures/remote_input_{}.jpg", name), std::ios::binary)
        .write(reinterpret_cast<const char*>(latest.bytes.data()), static_cast<std::streamsize>(latest.bytes.size()));
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
    POINT cursor_before{};
    GetCursorPos(&cursor_before);
    Checks check;

    LatestJpeg latest;
    auto fake_send = [&](const proto::FrameHeader&, std::span<const std::uint8_t> payload) {
        std::lock_guard lock(latest.mutex);
        latest.bytes.assign(payload.begin(), payload.end());
        ++latest.count;
        return true;
    };

    // The line: left to right across Paint's canvas, which sits roughly at x 0.07..0.66, y 0.17..0.74
    // of a maximized 1920x1080 Paint window.
    constexpr double kLineY = 0.40, kLineX0 = 0.20, kLineX1 = 0.50;
    {
        std::unique_ptr<kvm::WindowShare> share;
        try {
            share = kvm::WindowShare::start(hwnd, fake_send, {});
        } catch (const std::exception& e) {
            kvm::log::error("{}", e.what());
            return 1;
        }
        kvm::RemoteInput input(share->window(), share->monitor_rect());
        settle();
        save(latest, "1_before");
        const int dark_before = dark_columns(latest, kLineY, kLineX0, kLineX1);
        kvm::log::info("dark columns on the line before drawing: {}", dark_before);

        // Draw: button down at the start, 40 moves, button up at the end. The window isn't the
        // foreground window yet; RemoteInput must make the first click count anyway.
        input.handle(proto::Message{proto::RemoteMouseMove{.x_norm = kLineX0, .y_norm = kLineY}});
        input.handle(proto::Message{proto::MouseButtonEvent{.button = proto::MouseButton::Left, .down = true, .x_norm = kLineX0, .y_norm = kLineY}});
        for (int i = 1; i <= 40; ++i) {
            input.handle(proto::Message{proto::RemoteMouseMove{.x_norm = kLineX0 + (kLineX1 - kLineX0) * i / 40.0, .y_norm = kLineY}});
            std::this_thread::sleep_for(10ms);
        }
        input.handle(proto::Message{proto::MouseButtonEvent{.button = proto::MouseButton::Left, .down = false, .x_norm = kLineX1, .y_norm = kLineY}});
        settle();
        save(latest, "2_drawn");
        const int dark_drawn = dark_columns(latest, kLineY, kLineX0, kLineX1);
        check(dark_drawn > dark_before + 200, std::format("mouse: the line shows up in the stream ({} dark columns)", dark_drawn));

        // Ctrl+Z through key messages.
        input.handle(proto::Message{proto::Key{.vk = VK_CONTROL, .down = true}});
        input.handle(proto::Message{proto::Key{.vk = 'Z', .down = true}});
        input.handle(proto::Message{proto::Key{.vk = 'Z', .down = false}});
        input.handle(proto::Message{proto::Key{.vk = VK_CONTROL, .down = false}});
        settle();
        save(latest, "3_undone");
        const int dark_undone = dark_columns(latest, kLineY, kLineX0, kLineX1);
        check(dark_undone <= dark_before + 20, std::format("keyboard: Ctrl+Z removed it ({} dark columns)", dark_undone));

        // A click on the title bar must not leave the window un-maximized: the viewer shows the
        // whole VDD, so the window has to keep filling it.
        input.handle(proto::Message{proto::MouseButtonEvent{.button = proto::MouseButton::Left, .down = true, .x_norm = 0.5, .y_norm = 0.01}});
        input.handle(proto::Message{proto::MouseButtonEvent{.button = proto::MouseButton::Left, .down = false, .x_norm = 0.5, .y_norm = 0.01}});
        std::this_thread::sleep_for(300ms);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL), &mi);
        check(IsZoomed(hwnd) && EqualRect(&mi.rcMonitor, &share->monitor_rect()),
              "after a title bar click the window still fills the VDD");

        check(!input.handle(proto::Key{.vk = VK_LWIN, .down = true}), "Windows key is refused");
        input.handle(proto::Key{.vk = VK_SHIFT, .down = true});
        input.release_all();
        check((GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0, "release_all() let go of a held Shift");
        check(!input.handle(proto::Message{proto::Ping{}}), "other messages are ignored");
    }  // input releases, share ends: window back, VDD off

    SetCursorPos(cursor_before.x, cursor_before.y);
    RECT after{};
    GetWindowRect(hwnd, &after);
    check(EqualRect(&before, &after) != FALSE, "window is back exactly where it was");

    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
