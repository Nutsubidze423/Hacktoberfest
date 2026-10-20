// SPDX-License-Identifier: Apache-2.0

#include "window_picker.h"

#include <dwmapi.h>

#include <cstdio>
#include <iostream>
#include <memory>

#pragma comment(lib, "dwmapi.lib")

namespace kvm {
namespace {

struct HandleCloser {
    void operator()(HANDLE h) const noexcept { if (h) CloseHandle(h); }
};
using unique_handle = std::unique_ptr<void, HandleCloser>;

bool is_cloaked(HWND hwnd) {
    // UWP apps on other virtual desktops / suspended store apps are "cloaked": visible but not shown.
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

std::wstring window_title(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring title(static_cast<size_t>(len) + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, title.data(), len + 1);
    title.resize(static_cast<size_t>(copied));
    return title;
}

std::wstring process_name(DWORD pid) {
    unique_handle proc{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!proc) return L"?";  // e.g. elevated process while we are not elevated
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    if (!QueryFullProcessImageNameW(proc.get(), 0, path, &size)) return L"?";
    std::wstring full(path, size);
    const auto slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? full : full.substr(slash + 1);
}

bool is_capturable(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;  // dialogs/popups owned by another window
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return false;
    if (is_cloaked(hwnd)) return false;
    if (GetWindowTextLengthW(hwnd) == 0) return false;
    if (hwnd == GetConsoleWindow()) return false;

    wchar_t cls[64];
    if (GetClassNameW(hwnd, cls, 64) && (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0))
        return false;  // desktop
    return true;
}

BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lparam) {
    auto* out = reinterpret_cast<std::vector<WindowInfo>*>(lparam);
    if (!is_capturable(hwnd)) return TRUE;

    WindowInfo info;
    info.hwnd = hwnd;
    info.title = window_title(hwnd);
    GetWindowThreadProcessId(hwnd, &info.pid);
    info.process_name = process_name(info.pid);
    GetWindowRect(hwnd, &info.rect);
    info.minimized = IsIconic(hwnd) != FALSE;
    out->push_back(std::move(info));
    return TRUE;
}

// std::wcout can't print non-ASCII titles (e.g. Georgian) reliably; write UTF-16 to the console directly.
void print_w(const std::wstring& s) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode)) {
        DWORD written = 0;
        WriteConsoleW(out, s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
    } else {
        std::fputs(to_utf8(s).c_str(), stdout);  // redirected to a file/pipe
    }
}

// Windows PowerShell prefixes piped stdin with a UTF-8 BOM; drop it and surrounding whitespace.
std::string trim_input(std::string s) {
    if (s.starts_with("\xEF\xBB\xBF")) s.erase(0, 3);
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

}  // namespace

std::string to_utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::vector<WindowInfo> list_capturable_windows() {
    std::vector<WindowInfo> windows;
    EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

std::optional<WindowInfo> pick_window_console() {
    const auto windows = list_capturable_windows();
    if (windows.empty()) {
        std::cout << "No capturable windows found.\n";
        return std::nullopt;
    }

    for (size_t i = 0; i < windows.size(); ++i) {
        const auto& w = windows[i];
        wchar_t line[128];
        swprintf_s(line, L"%3zu) [%5ldx%-5ld]%ls ", i + 1, w.rect.right - w.rect.left, w.rect.bottom - w.rect.top,
                   w.minimized ? L" (min)" : L"");
        print_w(line + w.title + L"  -  " + w.process_name + L"\n");
    }

    for (;;) {
        std::cout << "Select window [1-" << windows.size() << "], or q to quit: " << std::flush;
        std::string input;
        if (!std::getline(std::cin, input)) return std::nullopt;
        input = trim_input(std::move(input));
        if (input.empty() || input == "q") return std::nullopt;
        try {
            const size_t idx = std::stoul(input);
            if (idx >= 1 && idx <= windows.size()) {
                const auto& chosen = windows[idx - 1];
                if (!IsWindow(chosen.hwnd)) {
                    std::cout << "That window was closed, pick another.\n";
                    continue;
                }
                return chosen;
            }
        } catch (const std::exception&) {
        }
        std::cout << "Invalid choice.\n";
    }
}

}  // namespace kvm
