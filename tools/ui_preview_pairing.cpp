// SPDX-License-Identifier: Apache-2.0

// ui_preview_pairing: shows the pairing question (client_b/pairing_dialog.h) as two PCs would see it,
// to check its wording and layout by hand. Prints what was answered.
//
//   ui_preview_pairing.exe [--seconds 55]

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string_view>

#include "kvm/dpi.h"
#include "pairing_dialog.h"

int wmain(int argc, wchar_t** argv) {
    kvm::display::enable_dpi_awareness();
    int seconds = 55;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--seconds" && i + 1 < argc) seconds = _wtoi(argv[++i]);
    }
    kvm::PairingDialog dialog;
    const auto answer = dialog.ask(nullptr, L"DESKTOP-1", L"042 917", std::chrono::seconds(seconds));
    std::printf("%s\n", !answer ? "closed" : *answer ? "pair yes" : "pair no");
    return 0;
}
