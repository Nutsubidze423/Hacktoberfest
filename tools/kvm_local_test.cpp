// SPDX-License-Identifier: Apache-2.0

// Hands-on KVM test on one PC, no network: the real hooks capture your mouse and
// keyboard, and a simulated B screen stands on the config's client_side.
//
// Push the cursor against that edge: control "moves to B" — A's cursor freezes,
// clicks/keys stop reaching A, and the log shows what would be sent to B
// (motion, buttons, wheel, keys) plus B's simulated cursor. Move back through
// B's opposite edge to return to A.
//
// Ctrl+Alt+Shift+Q returns control to A and removes the hooks (and ends the test).
// Usage: kvm_local_test.exe [--side left|right] [--config <path>] [--log <file>]

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "kvm/input_capture.h"
#include "kvm/config.h"
#include "kvm/dpi.h"
#include "kvm/log.h"

namespace display = kvm::display;
namespace proto = kvm::proto;

namespace {

std::atomic<bool> g_running{true};

BOOL WINAPI on_console_ctrl(DWORD) {
    g_running = false;
    return TRUE;
}

// Stand-in for B: a screen the size of A's primary monitor with its own cursor.
struct FakeB {
    kvm::host::InputCapture* capture = nullptr;
    display::Side host_side = display::Side::Left;  // where A is, seen from B
    int w = 1920;
    int h = 1080;
    bool active = false;
    int x = 0;
    int y = 0;
    std::chrono::steady_clock::time_point next_report{};

    bool receive(const proto::Message& msg) {
        if (const auto* enter = std::get_if<proto::FocusEnter>(&msg)) {
            active = true;
            const auto p = display::point_on_outer_edge({fake_monitor()}, host_side, enter->y_norm);
            const auto in = display::inward(p, host_side, 2);
            x = in.x;
            y = in.y;
            kvm::log::info("[B] focus_enter y_norm {:.3f} -> B cursor at ({}, {})", enter->y_norm, x, y);
        } else if (std::get_if<proto::FocusLeave>(&msg)) {
            active = false;
            kvm::log::info("[B] focus_leave from A: B releases everything");
        } else if (const auto* m = std::get_if<proto::MouseMove>(&msg)) {
            if (active) move(m->dx, m->dy);
        } else if (const auto* b = std::get_if<proto::MouseButtonEvent>(&msg)) {
            kvm::log::info("[B] mouse_button {} {}", nlohmann::json(b->button).get<std::string>(), b->down ? "down" : "up");
        } else if (const auto* wh = std::get_if<proto::MouseWheel>(&msg)) {
            kvm::log::info("[B] mouse_wheel {} {}", wh->delta, wh->horizontal ? "horizontal" : "vertical");
        } else if (const auto* k = std::get_if<proto::Key>(&msg)) {
            kvm::log::info("[B] key vk 0x{:02X} scan 0x{:02X}{} {}", k->vk, k->scan, k->extended ? " ext" : "",
                           k->down ? "down" : "up");
        }
        return true;
    }

    display::Monitor fake_monitor() const {
        display::Monitor m;
        m.rect = {0, 0, w, h};
        m.primary = true;
        return m;
    }

    void move(int dx, int dy) {
        const bool towards_a = host_side == display::Side::Left ? dx < 0 : dx > 0;
        const auto t = display::outer_edge_position({fake_monitor()}, {x, y}, host_side);
        if (towards_a && t) {
            active = false;
            kvm::log::info("[B] cursor left B at y_norm {:.3f} -> focus_leave to A", *t);
            capture->on_focus_leave({.y_norm = *t});
            return;
        }
        x = std::clamp(x + dx, 0, w - 1);
        y = std::clamp(y + dy, 0, h - 1);
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_report) {
            kvm::log::info("[B] cursor ({}, {})", x, y);
            next_report = now + std::chrono::milliseconds(200);
        }
    }
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
    display::enable_dpi_awareness();
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);

    std::optional<std::filesystem::path> config_path;
    std::optional<std::string> side_arg;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == L"--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == L"--log" && i + 1 < argc) {
            kvm::log::open_file(argv[++i]);
        } else if (arg == L"--side" && i + 1 < argc) {
            side_arg.emplace();
            for (const wchar_t c : std::wstring_view(argv[++i])) side_arg->push_back(c < 0x80 ? static_cast<char>(c) : '?');
        }
    }

    std::string side_name = side_arg.value_or("right");
    if (!side_arg) {
        try {
            side_name = kvm::load_config(config_path).client_side;
        } catch (const std::exception& e) {
            kvm::log::warn("config: {} - using client_side right", e.what());
        }
    }
    const auto side = display::parse_side(side_name);
    if (!side || (*side != display::Side::Left && *side != display::Side::Right)) {
        kvm::log::error("client_side must be left or right, got '{}'", side_name);
        return 1;
    }

    const auto primary = display::primary_monitor();
    FakeB b;
    b.host_side = display::opposite(*side);
    b.w = primary.rect.width();
    b.h = primary.rect.height();

    kvm::host::InputCapture capture({.client_side = *side});
    b.capture = &capture;
    if (!capture.start()) return 1;
    capture.set_client([&](const proto::Message& m) { return b.receive(m); });

    kvm::log::info("kvm_local_test: simulated B ({}x{}) on the {} of A. Push the cursor against that edge.",
                   b.w, b.h, display::to_string(*side));
    kvm::log::info("kvm_local_test: Ctrl+Alt+Shift+Q = emergency exit (ends the test), Ctrl+C = quit while on A.");

    while (g_running && !capture.disabled()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    capture.stop();
    kvm::log::info("kvm_local_test: bye");
    return 0;
}
