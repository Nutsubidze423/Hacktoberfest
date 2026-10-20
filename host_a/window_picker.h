// SPDX-License-Identifier: Apache-2.0

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace kvm {

// A top-level window that can be captured and streamed to B.
struct WindowInfo {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring process_name;  // e.g. L"Photoshop.exe"
    DWORD pid = 0;
    RECT rect{};                // window rect in physical pixels (process must be per-monitor DPI aware)
    bool minimized = false;     // WGC delivers no new frames while minimized
};

// Visible, titled, non-tool, non-cloaked top-level windows, in Z-order.
// Excludes the calling process's own console window.
std::vector<WindowInfo> list_capturable_windows();

// Prints the window list to the console and lets the user pick one by number.
// Returns std::nullopt if the list is empty or the user enters nothing / 'q'.
std::optional<WindowInfo> pick_window_console();

// UTF-8 conversion for logs and protocol fields (e.g. stream_start.title).
std::string to_utf8(const std::wstring& s);

}  // namespace kvm
