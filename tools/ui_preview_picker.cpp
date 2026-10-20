// SPDX-License-Identifier: Apache-2.0

// Shows the "Connect to another PC" card (client_b/device_picker.h) with sample PCs, at the mouse
// cursor (move the mouse where it should open first). Clicks are printed; it quits when the card closes.
// Usage: ui_preview_picker.exe [--empty] [--connected]

#include <windows.h>

#include <cstdio>
#include <string_view>
#include <vector>

#include "device_picker.h"
#include "kvm/dpi.h"

int wmain(int argc, wchar_t** argv) {
    kvm::display::enable_dpi_awareness();
    bool empty = false, connected = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == L"--empty") empty = true;
        if (arg == L"--connected") connected = true;
    }
    using State = kvm::DevicePicker::Row::State;
    std::vector<kvm::DevicePicker::Row> rows;
    if (!empty) {
        rows = {{.name = L"DESKTOP-9THG8KK",
                 .ip = L"172.20.10.4",
                 .state = connected ? State::Connected : State::Free,
                 .auto_connect = true},
                {.name = L"luka-laptop", .ip = L"172.20.10.7", .state = State::Busy},
                {.name = L"sandro-pc", .ip = L"172.20.10.9", .state = State::Free, .paired = false},
                {.name = L"office-desktop", .state = State::Offline, .auto_connect = true}};
    }
    kvm::DevicePicker picker;
    picker.show(rows, {.connect = [](std::size_t i) { std::printf("connect %zu\n", i); },
                       .toggle_auto = [](std::size_t i) { std::printf("auto %zu\n", i); },
                       .disconnect = [] { std::printf("disconnect\n"); }});
    MSG msg;
    while (picker.is_open() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
