// SPDX-License-Identifier: Apache-2.0

// viewer_h264_test: the viewer shows H.264 (roadmap P2.4b). In one process, a ViewerWindow and its
// VideoReceiver on a TLS video channel over 127.0.0.1 get the sample stream (tools/data/h264_sample.h264:
// red, green, blue and gray quadrants, 10 pictures) four times over at 30 fps, with one frame lost in
// the third round. Twice: on the GPU (DXVA decode, the video processor), if this PC has one, and in
// software (WARP, software decode, conversion on the CPU), as on a PC without a GPU. It checks:
//   - the first picture appears within the first round, and from then on every frame is shown, as
//     "h264" at 320x240, except after the lost one: from there the viewer waits for the next keyframe
//     (the next round), so nothing broken is shown (kvm/video_stats.h);
//   - the window shows the quadrants' colours (read back with PrintWindow, so another window on top
//     doesn't matter).
//
//   viewer_h264_test.exe <h264_sample.h264> [--hold <seconds>]   (exit code 0 = all passed)
//   --hold keeps each window open that long at the end, to look at it.

#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "kvm/annexb.h"
#include "kvm/log.h"
#include "kvm/net.h"
#include "kvm/video.h"
#include "kvm/video_stats.h"
#include "side_channel_util.h"
#include "video_receiver.h"
#include "viewer_window.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) {
        ++g_failures;
    }
}

constexpr int kRounds = 4, kLostRound = 2, kLostIndex = 2;  // round 3 loses its third frame

struct Unit {
    std::vector<std::uint8_t> bytes;
    bool keyframe = false;
};

std::vector<Unit> load(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<std::uint8_t> stream((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<Unit> units;
    for (const auto& [first, second] : kvm::annexb::access_units(stream)) {
        Unit u{.bytes = {stream.begin() + static_cast<std::ptrdiff_t>(first),
                         stream.begin() + static_cast<std::ptrdiff_t>(second)}};
        for (const auto& nal : kvm::annexb::split(u.bytes)) u.keyframe |= nal.type == kvm::annexb::kIdrSlice;
        units.push_back(std::move(u));
    }
    return units;
}

// Runs the message loop until `until` returns true or `timeout` passes.
template <class F>
bool pump(F until, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!until()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    return true;
}

// The frames the CSV says were shown: seq -> "codec widthxheight", and the average decode (with the
// conversion to BGRA) and present times.
struct Shown {
    std::map<std::uint32_t, std::string> frames;
    double decode_ms = 0;
    double present_ms = 0;
};

Shown shown_frames(const fs::path& csv) {
    Shown out;
    std::ifstream in(csv);
    std::string line;
    std::getline(in, line);  // header: t_ms,layer,stream,seq,codec,bytes,width,height,...
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        for (std::string field; std::getline(ss, field, ',');) f.push_back(field);
        if (f.size() < 11 || f[1] != "window") continue;
        out.frames[static_cast<std::uint32_t>(std::stoul(f[3]))] = f[4] + " " + f[6] + "x" + f[7];
        out.decode_ms += std::stod(f[9]);
        out.present_ms += std::stod(f[10]);
    }
    if (!out.frames.empty()) {
        out.decode_ms /= static_cast<double>(out.frames.size());
        out.present_ms /= static_cast<double>(out.frames.size());
    }
    return out;
}

// The colour at client point (x, y) of the window's own content.
COLORREF pixel_at(HWND hwnd, int x, int y) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    const HDC screen = GetDC(nullptr);
    const HDC mem = CreateCompatibleDC(screen);
    const HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    COLORREF out = CLR_INVALID;
    if (bmp && mem) {
        const HGDIOBJ old = SelectObject(mem, bmp);
        // PW_RENDERFULLCONTENT: what DWM shows, flip-model swapchains included.
        if (PrintWindow(hwnd, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT) && x >= 0 && y >= 0 && x < w && y < h) {
            const auto* px = static_cast<const std::uint8_t*>(bits) + (static_cast<std::size_t>(y) * w + x) * 4;
            out = RGB(px[2], px[1], px[0]);
        }
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return out;
}

bool near_colour(COLORREF got, int r, int g, int b) {
    constexpr int kTolerance = 16;
    return got != CLR_INVALID && std::abs(GetRValue(got) - r) <= kTolerance &&
           std::abs(GetGValue(got) - g) <= kTolerance && std::abs(GetBValue(got) - b) <= kTolerance;
}

void run(bool use_gpu, const std::vector<Unit>& units, const fs::path& csv, int hold_s) {
    const std::string mode = use_gpu ? "GPU" : "software";
    std::printf("\n-- %s --\n", mode.c_str());
    kvm::video_stats().open_csv(csv);

    kvm::ViewerWindow viewer;
    check(viewer.create(true, use_gpu), mode + ": viewer window");
    const auto gpu = viewer.gpu();
    if (use_gpu && (!gpu || !gpu->has_video())) {
        std::printf("       no GPU with video support here: skipped\n");
        return;
    }
    viewer.set_link_state(kvm::LinkState::Linked);
    viewer.toggle_hud();  // off: it covers the right of the picture
    viewer.start_stream({.hwnd = 1, .title = "viewer_h264_test", .width = 320, .height = 240});

    // The video channel: TLS over 127.0.0.1, the owner's side accepted by the test itself.
    std::uint16_t port = 0;
    const SOCKET listener = kvm::test::listen_loopback(port);
    const kvm::test::Session session;
    std::unique_ptr<kvm::net::FrameChannel> owner;
    std::thread acceptor([&] {
        if (auto a = kvm::test::admit_socket(accept(listener, nullptr, nullptr), session, "video", "viewer")) {
            owner = std::make_unique<kvm::net::FrameChannel>(std::move(*a));
        }
    });
    auto channel = kvm::net::connect_video("127.0.0.1", port, session.client, session.host.id(), session.token);
    acceptor.join();
    closesocket(listener);
    check(owner && channel, mode + ": video channel");
    if (!owner || !channel) return;

    std::set<std::uint32_t> expected;  // what should be shown
    {
        kvm::VideoReceiver receiver(*channel, viewer);
        receiver.set_clock_offset_us(0);
        std::atomic<bool> done = false;
        std::thread sender([&] {
            std::uint32_t seq = 0;
            auto next = std::chrono::steady_clock::now();
            for (int round = 0; round < kRounds; ++round) {
                for (std::size_t i = 0; i < units.size(); ++i, ++seq) {
                    const bool lost = round == kLostRound && i == kLostIndex;
                    // After the lost frame the rest of the round refers to it: not shown.
                    if (!(round == kLostRound && i >= kLostIndex)) expected.insert(seq);
                    if (!lost) {
                        owner->send_frame({.seq = seq,
                                           .t_capture_us = kvm::steady_now_us(),
                                           .width = 320,
                                           .height = 240,
                                           .codec = "h264",
                                           .keyframe = units[i].keyframe},
                                          units[i].bytes);
                    }
                    next += std::chrono::microseconds(1'000'000 / 30);
                    std::this_thread::sleep_until(next);
                }
            }
            done = true;
        });
        pump([&] { return done.load(); }, 20s);
        pump([] { return false; }, 700ms);  // the last frames reach the screen
        sender.join();

        const auto rect = viewer.picture_rect();
        check(rect.has_value(), mode + ": a picture is drawn");
        if (rect) {
            const int w = rect->right - rect->left, h = rect->bottom - rect->top;
            const auto at = [&](double fx, double fy) {
                return pixel_at(viewer.hwnd(), rect->left + static_cast<int>(w * fx),
                                rect->top + static_cast<int>(h * fy));
            };
            const COLORREF red = at(0.25, 0.25), green = at(0.75, 0.25), blue = at(0.25, 0.75), gray = at(0.75, 0.75);
            const bool colours = near_colour(red, 255, 0, 0) && near_colour(green, 0, 255, 0) &&
                                 near_colour(blue, 0, 0, 255) && near_colour(gray, 128, 128, 128);
            if (!colours) {
                std::printf("       got red %06lX, green %06lX, blue %06lX, gray %06lX (0xBBGGRR)\n", red, green, blue,
                            gray);
            }
            check(colours, mode + ": the window shows red, green, blue and gray quadrants (within 16 levels)");
            check(std::abs(w * 3 - h * 4) <= 4, mode + ": drawn at 4:3, like the stream");
        }
        if (hold_s > 0) pump([] { return false; }, std::chrono::seconds(hold_s));
    }
    owner->close();

    kvm::video_stats().flush();
    const auto result = shown_frames(csv);
    const auto& shown = result.frames;
    std::printf("       decode + BGRA %.1f ms, present %.1f ms (averages)\n", result.decode_ms, result.present_ms);
    std::set<std::uint32_t> seqs;
    bool all_h264 = true;
    for (const auto& [seq, what] : shown) {
        seqs.insert(seq);
        all_h264 = all_h264 && what == "h264 320x240";
    }
    std::vector<std::uint32_t> missing, extra;
    std::set_difference(expected.begin(), expected.end(), seqs.begin(), seqs.end(), std::back_inserter(missing));
    std::set_difference(seqs.begin(), seqs.end(), expected.begin(), expected.end(), std::back_inserter(extra));
    if (!missing.empty()) {
        std::string list;
        for (const auto m : missing) list += " " + std::to_string(m);
        std::printf("       not shown:%s\n", list.c_str());
    }
    // At the start the decoder is being set up and the window sized for the stream: the frames that
    // arrive meanwhile are decoded, and only the newest is shown. From the first picture on, a frame
    // that arrives while the window is busy drawing is replaced by the next one: a few may be missing
    // on a slow PC. None may be shown that refers to the lost frame.
    const std::uint32_t first = seqs.empty() ? 0 : *seqs.begin();
    const auto later = std::count_if(missing.begin(), missing.end(), [&](std::uint32_t m) { return m > first; });
    check(extra.empty(), mode + ": nothing shown between the lost frame and the next keyframe (" +
                             std::to_string(extra.size()) + " shown)");
    check(!seqs.empty() && first < units.size(),
          mode + ": the first picture within the first round (frame " + std::to_string(first) + ")");
    check(later <= 2, mode + ": " + std::to_string(shown.size()) + " of " + std::to_string(expected.size()) +
                          " frames shown; after the first, " + std::to_string(later) +
                          " replaced by a newer one before it was drawn");
    check(all_h264 && !shown.empty(), mode + ": every frame shown is h264, 320x240");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: viewer_h264_test.exe <h264_sample.h264> [--hold <seconds>]\n");
        return 2;
    }
    int hold_s = 0;
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--hold") hold_s = std::atoi(argv[i + 1]);
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto units = load(argv[1]);
    check(units.size() == 10 && units.front().keyframe, std::to_string(units.size()) + " access units in the sample");
    if (units.empty()) return 1;

    const fs::path dir = fs::temp_directory_path() / ("sarke_viewer_h264_" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(dir);
    run(true, units, dir / "gpu.csv", hold_s);
    run(false, units, dir / "software.csv", hold_s);
    std::error_code ec;
    fs::remove_all(dir, ec);

    std::printf("\n%s\n", g_failures == 0 ? "all passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
