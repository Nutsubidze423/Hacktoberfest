// SPDX-License-Identifier: Apache-2.0

// Shows Sarke's notification card (client_b/toast.h) at the top-right of the main screen.
// Usage: ui_preview_toast.exe [--kind connected|lost|info] [--title <text>] [--text <text>] [--hold <ms>]
//                             [--motion on|off]   (default: Windows' "Animation effects" setting)
//        ui_preview_toast.exe --sequence     (connected, then lost, then info, one after another)
// Quits when the last card has gone. Hover holds a card; a click or × closes it.

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kvm/dpi.h"
#include "toast.h"

#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup")

namespace {

using kvm::Toast;
constexpr UINT_PTR kPollTimer = 1;

std::optional<Toast::Kind> parse_kind(std::wstring_view s) {
    if (s == L"connected") return Toast::Kind::Connected;
    if (s == L"lost") return Toast::Kind::Lost;
    if (s == L"info") return Toast::Kind::Info;
    return std::nullopt;
}

Toast::Content sample(Toast::Kind kind) {
    switch (kind) {
    case Toast::Kind::Connected:
        return {.kind = kind,
                .tag = L"Link",
                .title = L"Connected to tabuladze",
                .text = L"172.20.10.2 · 1920×1200 · 150%"};
    case Toast::Kind::Lost:
        return {.kind = kind, .tag = L"Link", .title = L"Connection lost", .text = L"reconnecting to 172.20.10.3"};
    default:
        return {.kind = kind,
                .tag = L"Window",
                .title = L"Photoshop is on tabuladze",
                .text = L"Ctrl+Alt+Shift+W brings it back"};
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    kvm::display::enable_dpi_awareness();

    std::vector<Toast::Content> queue;
    Toast::Content one = sample(Toast::Kind::Connected);
    bool sequence = false;
    std::chrono::milliseconds hold = Toast::kDefaultHold;
    std::optional<bool> motion;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == L"--kind" && i + 1 < argc) {
            const auto kind = parse_kind(argv[++i]);
            if (!kind) return 2;
            one = sample(*kind);
        } else if (arg == L"--title" && i + 1 < argc) {
            one.title = argv[++i];
        } else if (arg == L"--text" && i + 1 < argc) {
            one.text = argv[++i];
        } else if (arg == L"--hold" && i + 1 < argc) {
            hold = std::chrono::milliseconds(_wtoi(argv[++i]));
        } else if (arg == L"--motion" && i + 1 < argc) {
            const std::wstring_view v = argv[++i];
            if (v != L"on" && v != L"off") return 2;
            motion = v == L"on";
        } else if (arg == L"--sequence") {
            sequence = true;
        } else {
            return 2;
        }
    }
    if (sequence) {
        queue = {sample(Toast::Kind::Connected), sample(Toast::Kind::Lost), sample(Toast::Kind::Info)};
    } else {
        queue = {one};
    }

    Toast toast;
    toast.set_motion(motion);
    std::size_t next = 0;
    const auto show_next = [&] {
        if (next >= queue.size()) return false;
        toast.show(queue[next++], [] { std::fputs("clicked\n", stderr); }, hold);
        return true;
    };
    if (!show_next()) return 1;

    // Poll: when a card has gone, show the next one or quit.
    SetTimer(nullptr, kPollTimer, 100, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.hwnd == nullptr && !toast.visible() && !show_next()) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
