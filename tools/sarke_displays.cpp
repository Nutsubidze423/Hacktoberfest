// SPDX-License-Identifier: Apache-2.0

// Sarke display arrangement: where the other PC stands next to this one (client_b/display_settings.h).
// Detects this PC's monitors live, shows the other PC (as last seen), writes config.json client_side.
//
// Usage: sarke_displays.exe [--config <path>] [--role host|client] [--peer <W>x<H>[@<scale>] <hostname>]
//   --role   default: guessed from config.json host_ip (this PC's address = host)
//   --peer   e.g. --peer 1920x1200@1.5 tabuladze (otherwise the last one kvm_host / kvm_client saw)

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "display_layout.h"
#include "display_settings.h"
#include "kvm/dpi.h"
#include "kvm/log.h"

// A window app: no console window when double-clicked (tools/CMakeLists builds console exes).
// Log lines still go to stderr when it is redirected.
#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup")

namespace {

std::string narrow(std::wstring_view s) {
    if (s.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

// "1920x1200" or "1920x1200@1.5".
bool parse_size(std::wstring_view s, kvm::layout::Peer& peer) {
    double scale = 1.0;
    int w = 0, h = 0;
    const int got = swscanf_s(std::wstring(s).c_str(), L"%dx%d@%lf", &w, &h, &scale);
    if (got < 2 || w <= 0 || h <= 0 || w > 32768 || h > 32768 || !(scale >= 0.5 && scale <= 8.0)) return false;
    peer.width = w;
    peer.height = h;
    peer.scale = scale;
    return true;
}

int usage() {
    std::fputs("usage: sarke_displays.exe [--config <path>] [--role host|client] [--peer <W>x<H>[@<scale>] <hostname>]\n",
               stderr);
    return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    kvm::display::enable_dpi_awareness();

    kvm::DisplaySettings::Options options;
    std::optional<kvm::layout::Peer> peer;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == L"--config" && i + 1 < argc) {
            options.config_path = std::filesystem::path(argv[++i]);
        } else if (arg == L"--role" && i + 1 < argc) {
            const std::wstring_view role = argv[++i];
            if (role == L"host") {
                options.role = kvm::layout::Role::Host;
            } else if (role == L"client") {
                options.role = kvm::layout::Role::Client;
            } else {
                return usage();
            }
        } else if (arg == L"--peer" && i + 2 < argc) {
            kvm::layout::Peer p;
            if (!parse_size(argv[++i], p)) return usage();
            p.hostname = narrow(argv[++i]);
            peer = p;
        } else {
            return usage();
        }
    }

    kvm::DisplaySettings window;
    if (peer) window.set_peer(*peer, /*connected=*/false);
    window.set_close_handler([] { PostQuitMessage(0); });
    if (!window.show(options)) {
        kvm::log::error("sarke_displays: could not open the window");
        return 1;
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
