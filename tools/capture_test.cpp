// capture_test: the A side of "send a window to B", without the network:
//   pick a window -> VDD on (1920x1080, right) -> window onto the VDD -> capture frames of the VDD
//   -> captures/frame_NN.bmp -> window back -> VDD off.
// The window always goes back and the VDD is always detached, also when a step fails.
//
//   capture_test.exe [--title <part of the title>] [--frames 10] [--seconds 10] [--wiggle]
//
// WGC only delivers a frame when something on the VDD changes. --wiggle moves the mouse in a small
// circle on the VDD so frames keep coming (the cursor is part of the capture).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "capture.h"
#include "kvm/dpi.h"
#include "kvm/log.h"
#include "kvm/net.h"
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

bool save_bmp(const std::filesystem::path& path, const kvm::CapturedFrame& f) {
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = f.width;
    info.biHeight = -f.height;  // top-down rows, like the capture
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;  // "BM"
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(f.bgra.size());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    out.write(reinterpret_cast<const char*>(f.bgra.data()), static_cast<std::streamsize>(f.bgra.size()));
    return static_cast<bool>(out);
}

// Average of R, G and B over a sample of pixels, 0..255. Near 0 means a black frame.
double mean_brightness(const kvm::CapturedFrame& f) {
    std::uint64_t sum = 0, n = 0;
    for (size_t i = 0; i + 3 < f.bgra.size(); i += 4 * 97) {
        sum += f.bgra[i] + f.bgra[i + 1] + f.bgra[i + 2];
        n += 3;
    }
    return n ? static_cast<double>(sum) / static_cast<double>(n) : 0.0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    kvm::display::enable_dpi_awareness();

    std::wstring title;
    size_t frames_wanted = 10;
    int seconds = 10;
    bool wiggle = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == L"--title" && has_value) title = argv[++i];
        else if (arg == L"--frames" && has_value) frames_wanted = static_cast<size_t>(std::wcstol(argv[++i], nullptr, 10));
        else if (arg == L"--seconds" && has_value) seconds = static_cast<int>(std::wcstol(argv[++i], nullptr, 10));
        else if (arg == L"--wiggle") wiggle = true;
    }

    if (!kvm::ScreenCapture::is_supported()) {
        kvm::log::error("Windows.Graphics.Capture is not supported on this Windows version");
        return 1;
    }
    const auto window = title.empty() ? kvm::pick_window_console() : window_with_title(title);
    if (!window) {
        if (!title.empty()) kvm::log::error("no window with \"{}\" in its title", kvm::to_utf8(title));
        return title.empty() ? 0 : 1;
    }
    kvm::log::info("window: \"{}\" ({})", kvm::to_utf8(window->title), kvm::to_utf8(window->process_name));

    auto vdd = kvm::VirtualDisplay::find();
    if (!vdd) {
        kvm::log::error("Virtual Display Driver not found");
        return 1;
    }
    kvm::WindowMover mover;
    struct Cleanup {
        kvm::WindowMover& mover;
        kvm::VirtualDisplay& vdd;
        HWND hwnd;
        ~Cleanup() {
            if (mover.sent_window()) mover.restore(hwnd);
            vdd.detach();
        }
    } cleanup{mover, *vdd, window->hwnd};

    Checks check;
    check(vdd->attach(1920, 1080, 60, kvm::display::Side::Right), "VDD attached, 1920x1080 on the right");
    const auto where = vdd->rect();
    if (!where) return 1;
    check(mover.send(window->hwnd, where->rect) == kvm::WindowMover::Result::Ok, "window sent to the VDD");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let it repaint at the new size

    kvm::LatestSlot<kvm::CapturedFrame> slot;
    std::unique_ptr<kvm::ScreenCapture> capture;
    try {
        capture = kvm::ScreenCapture::for_monitor(where->monitor);
        capture->start(slot);
    } catch (const std::exception& e) {
        kvm::log::error("{}", e.what());
        return 1;
    }

    POINT cursor_before{};
    GetCursorPos(&cursor_before);
    std::atomic<bool> wiggling = wiggle;
    std::thread wiggler([&] {
        const int cx = (where->rect.left + where->rect.right) / 2, cy = (where->rect.top + where->rect.bottom) / 2;
        for (double a = 0; wiggling; a += 0.3) {
            SetCursorPos(cx + static_cast<int>(60 * std::cos(a)), cy + static_cast<int>(60 * std::sin(a)));
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    });

    std::vector<kvm::CapturedFrame> frames;
    std::vector<std::int64_t> waits_us;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (frames.size() < frames_wanted) {
        const auto left = deadline - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero()) break;
        auto frame = slot.take_for(left);
        if (!frame) break;
        waits_us.push_back(kvm::steady_now_us() - frame->t_capture_us);
        frames.push_back(std::move(*frame));
    }
    wiggling = false;
    wiggler.join();
    if (wiggle) SetCursorPos(cursor_before.x, cursor_before.y);
    capture->stop();

    std::filesystem::create_directories("captures");
    for (size_t i = 0; i < frames.size(); ++i) save_bmp(std::format("captures/frame_{:02}.bmp", i), frames[i]);

    const int vdd_w = where->rect.right - where->rect.left, vdd_h = where->rect.bottom - where->rect.top;
    check(!frames.empty(), std::format("{} of {} frames in {} s", frames.size(), frames_wanted, seconds));
    check(std::all_of(frames.begin(), frames.end(),
                      [&](const auto& f) { return f.width == vdd_w && f.height == vdd_h; }),
          std::format("every frame is {}x{} like the VDD", vdd_w, vdd_h));
    check(std::all_of(frames.begin(), frames.end(),
                      [](const auto& f) {
                          return f.stride == f.width * 4 && f.bgra.size() == static_cast<size_t>(f.stride) * f.height;
                      }),
          "stride == width * 4, buffer == stride * height");
    if (!frames.empty()) {
        const double brightness = mean_brightness(frames.back());
        check(brightness > 5.0, std::format("last frame is not black (mean brightness {:.0f})", brightness));
        const double span_s = static_cast<double>(frames.back().t_capture_us - frames.front().t_capture_us) / 1e6;
        const auto max_wait = *std::max_element(waits_us.begin(), waits_us.end());
        kvm::log::info("{:.1f} frames/s, WGC -> consumer at most {:.2f} ms, {} frame(s) replaced unread",
                       span_s > 0 ? static_cast<double>(frames.size() - 1) / span_s : 0.0,
                       static_cast<double>(max_wait) / 1000.0, slot.overwritten());
        kvm::log::info("saved captures/frame_00.bmp .. frame_{:02}.bmp", frames.size() - 1);
    }

    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
