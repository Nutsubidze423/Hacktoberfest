// SPDX-License-Identifier: Apache-2.0

// Shows the "Paired PCs" card (client_b/paired_window.h) with sample PCs, at the mouse cursor (move
// the mouse where it should open first). Auto clicks are printed; Unpair asks for confirmation (the
// real dialog) and removes the row. It quits when the card closes.
// Usage: ui_preview_paired.exe [--empty]

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <string_view>
#include <vector>

#include "kvm/dpi.h"
#include "paired_window.h"
#include "pairing_dialog.h"

int wmain(int argc, wchar_t** argv) {
    kvm::display::enable_dpi_awareness();
    const bool empty = argc > 1 && std::wstring_view(argv[1]) == L"--empty";
    std::vector<kvm::PairedWindow::Row> rows;
    if (!empty) {
        rows = {{.name = L"DESKTOP-9THG8KK",
                 .fingerprint = L"1A2B-3C4D-5E6F-7081",
                 .seen = L"Connected",
                 .connected = true,
                 .auto_connect = true},
                {.name = L"luka-laptop", .fingerprint = L"9C0D-E1F2-0314-2536", .seen = L"Seen 3 h ago"},
                {.name = L"office-desktop",
                 .fingerprint = L"47A8-B9CA-DBEC-FD0E",
                 .seen = L"Not seen yet",
                 .auto_connect = true}};
    }
    kvm::PairedWindow window;
    window.show(rows, {.toggle_auto = [](std::size_t i) { std::printf("auto %zu\n", i); },
                       .unpair =
                           [&](std::size_t i) {
                               if (i >= rows.size() || !kvm::confirm_unpair(window.hwnd(), rows[i].name)) return;
                               std::printf("unpair %zu\n", i);
                               rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(i));
                               window.update(rows);
                           }});
    MSG msg;
    while (window.is_open() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
