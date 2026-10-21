// SPDX-License-Identifier: Apache-2.0

// capture_test: the A side of "send a window to B", without the network:
//   pick a window -> VDD on (B's size, on the config.json client_side) -> window onto the VDD
//   -> capture frames of the VDD -> captures/frame_NN.jpg -> window back -> VDD off.
// The window always goes back and the VDD is always detached, also when a step fails.
//
//   capture_test.exe [--title <part of the title>] [--frames 10] [--seconds 10] [--size 1920x1080]
//                    [--wiggle] [--menu] [--bmp]
//
// WGC only delivers a frame when something on the VDD changes. --wiggle moves the mouse in a small
// circle on the VDD so frames keep coming (the cursor is part of the capture). --menu opens the
// window's system menu (Alt+Space) while capturing, to show that popups reach B too. --bmp also
// saves the raw frames as .bmp (input for jpeg_test).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "capture.h"
#include "jpeg_encoder.h"
#include "kvm/config.h"
#include "kvm/dpi.h"
#include "kvm/gpu_device.h"
#include "kvm/log.h"
#include "kvm/net.h"
#include "nv12_converter.h"
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

void write_file(const std::filesystem::path& path, const void* data, size_t size) {
    std::ofstream(path, std::ios::binary).write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

void save_bmp(const std::filesystem::path& path, const kvm::CapturedFrame& f) {
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

void press_escape() {
    INPUT keys[2]{};
    keys[0].type = keys[1].type = INPUT_KEYBOARD;
    keys[0].ki.wVk = keys[1].ki.wVk = VK_ESCAPE;
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, keys, sizeof(INPUT));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    kvm::display::enable_dpi_awareness();

    kvm::Config config;
    try {
        config = kvm::load_config();
    } catch (const std::exception& e) {
        kvm::log::warn("config.json: {}; using defaults", e.what());
    }

    std::wstring title;
    size_t frames_wanted = 10;
    int seconds = 10;
    int width = 1920, height = 1080;
    bool wiggle = false, menu = false, bmp = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == L"--title" && has_value)
            title = argv[++i];
        else if (arg == L"--frames" && has_value)
            frames_wanted = static_cast<size_t>(std::wcstol(argv[++i], nullptr, 10));
        else if (arg == L"--seconds" && has_value)
            seconds = static_cast<int>(std::wcstol(argv[++i], nullptr, 10));
        else if (arg == L"--size" && has_value)
            swscanf_s(argv[++i], L"%dx%d", &width, &height);
        else if (arg == L"--wiggle")
            wiggle = true;
        else if (arg == L"--menu")
            menu = true;
        else if (arg == L"--bmp")
            bmp = true;
    }
    const auto side = kvm::display::parse_side(config.client_side).value_or(kvm::display::Side::Right);

    if (!kvm::ScreenCapture::is_supported()) {
        kvm::log::error("Windows.Graphics.Capture is not supported on this Windows version");
        return 1;
    }
    const auto window = title.empty() ? kvm::pick_window_console() : window_with_title(title);
    if (!window) {
        if (!title.empty()) kvm::log::error("no window with \"{}\" in its title", kvm::to_utf8(title));
        return title.empty() ? 0 : 1;
    }
    const HWND hwnd = window->hwnd;
    kvm::log::info("window: \"{}\" ({})", kvm::to_utf8(window->title), kvm::to_utf8(window->process_name));

    auto vdd = kvm::VirtualDisplay::find();
    if (!vdd) {
        kvm::log::error("Virtual Display Driver not found");
        return 1;
    }
    RECT rect_before{};
    GetWindowRect(hwnd, &rect_before);

    kvm::WindowMover mover;
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
    check(vdd->attach(width, height, 60, side),
          std::format("VDD attached, {}x{} on the {}", width, height, kvm::display::to_string(side)));
    const auto where = vdd->rect();
    if (!where) return 1;
    check(mover.send(hwnd, where->rect) == kvm::WindowMover::Result::Ok, "window sent to the VDD");
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
    if (menu) {
        // The window menu is a separate popup window, exactly what a window capture would miss.
        SetForegroundWindow(hwnd);
        PostMessageW(hwnd, WM_SYSCOMMAND, SC_KEYMENU, VK_SPACE);
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

    kvm::JpegEncoder jpeg(config.jpeg_quality);
    std::vector<std::uint8_t> encoded;
    std::vector<double> encode_ms;
    std::vector<kvm::CapturedFrame> frames;
    std::filesystem::create_directories("captures");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (frames.size() < frames_wanted) {
        const auto left = deadline - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero()) break;
        auto frame = slot.take_for(left);
        if (!frame) break;
        if (jpeg.encode(*frame, encoded)) {
            encode_ms.push_back(jpeg.last_encode_ms());
            write_file(std::format("captures/frame_{:02}.jpg", frames.size()), encoded.data(), encoded.size());
        }
        if (bmp) save_bmp(std::format("captures/frame_{:02}.bmp", frames.size()), *frame);
        frames.push_back(std::move(*frame));
    }
    wiggling = false;
    wiggler.join();
    if (wiggle) SetCursorPos(cursor_before.x, cursor_before.y);
    if (menu) press_escape();  // close the window menu
    capture->stop();

    const int vdd_w = where->rect.right - where->rect.left, vdd_h = where->rect.bottom - where->rect.top;
    check(!frames.empty(), std::format("{} of {} frames in {} s", frames.size(), frames_wanted, seconds));
    check(
        std::all_of(frames.begin(), frames.end(), [&](const auto& f) { return f.width == vdd_w && f.height == vdd_h; }),
        std::format("every frame is {}x{} like the VDD", vdd_w, vdd_h));
    check(encode_ms.size() == frames.size(), std::format("{} JPEG(s) written to captures/", encode_ms.size()));
    if (!frames.empty()) {
        check(mean_brightness(frames.back()) > 5.0, "last frame is not black");
        const double avg = std::accumulate(encode_ms.begin(), encode_ms.end(), 0.0) /
                           static_cast<double>(std::max<size_t>(1, encode_ms.size()));
        kvm::log::info("JPEG quality {}: {:.2f} ms per frame on average", jpeg.quality(), avg);
    }

    // The same monitor with the frames left on the GPU and turned into NV12 there (roadmap P2.3a).
    if (auto gpu = kvm::GpuDevice::create()) {
        kvm::Nv12Converter nv12(gpu);
        std::atomic<int> gpu_frames = 0, converted = 0;
        std::atomic<int> nv12_w = 0, nv12_h = 0;
        std::unique_ptr<kvm::ScreenCapture> on_gpu;
        try {
            on_gpu = kvm::ScreenCapture::for_monitor(where->monitor, true, gpu);
            on_gpu->start_gpu([&](kvm::GpuFrame&& f) {  // one callback at a time
                ++gpu_frames;
                if (nv12.convert(f.texture.Get(), f.width, f.height)) {
                    ++converted;
                    nv12_w = nv12.width();
                    nv12_h = nv12.height();
                }
            });
        } catch (const std::exception& e) {
            kvm::log::error("{}", e.what());
        }
        // WGC sends a frame when something changes: move the cursor over the monitor.
        const int cx = (where->rect.left + where->rect.right) / 2, cy = (where->rect.top + where->rect.bottom) / 2;
        for (int i = 0; i < 100 && gpu_frames < 10; ++i) {
            SetCursorPos(cx + static_cast<int>(60 * std::cos(i * 0.3)), cy + static_cast<int>(60 * std::sin(i * 0.3)));
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        SetCursorPos(cursor_before.x, cursor_before.y);
        if (on_gpu) on_gpu->stop();
        check(gpu_frames > 0 && converted == gpu_frames && nv12_w == (vdd_w & ~1) && nv12_h == (vdd_h & ~1),
              std::format("on the GPU ({}): {} frames captured, {} turned into {}x{} NV12", gpu->adapter_name(),
                          gpu_frames.load(), converted.load(), nv12_w.load(), nv12_h.load()));
    } else {
        kvm::log::warn("no hardware GPU with video support: the GPU path isn't checked");
    }

    // Back to how A looked before the test.
    check(mover.restore(hwnd), "window restored");
    RECT rect_after{};
    GetWindowRect(hwnd, &rect_after);
    check(EqualRect(&rect_before, &rect_after) != FALSE, "window is exactly where it was");
    check(vdd->detach() && !vdd->attached(), "VDD detached");

    if (check.failed) {
        kvm::log::error("{} check(s) failed", check.failed);
        return 1;
    }
    kvm::log::info("all passed");
    return 0;
}
