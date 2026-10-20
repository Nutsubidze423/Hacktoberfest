// SPDX-License-Identifier: Apache-2.0

#include "tray_icon.h"

#include <windowsx.h>
#include <shellapi.h>

#include <memory>
#include <mutex>
#include <type_traits>

#include "kvm/log.h"
#include "ui_tokens.h"

#pragma comment(lib, "shell32.lib")

namespace kvm {

namespace {

constexpr wchar_t kClassName[] = L"KvmTrayWindow";
constexpr UINT kTrayCallbackMsg = WM_APP + 10;
constexpr UINT kIconId = 1;
constexpr UINT kMenuShowViewer = 1;
constexpr UINT kMenuQuit = 2;
constexpr UINT kMenuDisplays = 3;

struct MenuDeleter {
    void operator()(HMENU menu) const { DestroyMenu(menu); }
};
using unique_menu = std::unique_ptr<std::remove_pointer_t<HMENU>, MenuDeleter>;

bool register_class_once(WNDPROC wnd_proc) {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [wnd_proc] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        ok = RegisterClassExW(&wc) != 0;
    });
    return ok;
}

template <std::size_t N>
void copy_text(wchar_t (&dst)[N], std::wstring_view src) {
    const std::size_t n = src.size() < N - 1 ? src.size() : N - 1;  // truncate, always terminate
    src.copy(dst, n);
    dst[n] = L'\0';
}

NOTIFYICONDATAW base_data(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kIconId;
    return nid;
}

}  // namespace

TrayIcon::~TrayIcon() {
    if (const HWND hwnd = hwnd_) {
        NOTIFYICONDATAW nid = base_data(hwnd);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        DestroyWindow(hwnd);
    }
}

bool TrayIcon::create(std::wstring app_name, Actions actions) {
    if (!register_class_once(&TrayIcon::wnd_proc)) {
        return false;
    }
    app_name_ = std::move(app_name);
    tooltip_ = app_name_;
    actions_ = std::move(actions);

    // A hidden top-level window (not message-only: it must receive the "TaskbarCreated" broadcast).
    hwnd_ = CreateWindowExW(0, kClassName, app_name_.c_str(), WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        return false;
    }
    taskbar_created_msg_ = RegisterWindowMessageW(L"TaskbarCreated");

    const UINT dpi = GetDpiForWindow(hwnd_);
    small_icon_ = ui::create_logo_icon(ui::small_icon_size(dpi));
    large_icon_ = ui::create_logo_icon(ui::large_icon_size(dpi));
    return add_icon();
}

bool TrayIcon::add_icon() {
    NOTIFYICONDATAW nid = base_data(hwnd_);
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayCallbackMsg;
    nid.hIcon = small_icon_.get();
    copy_text(nid.szTip, tooltip_);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        log::warn("tray: Shell_NotifyIcon(NIM_ADD) failed");
        return false;
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    return true;
}

void TrayIcon::set_state_word(std::wstring_view word) {
    tooltip_ = app_name_ + ui::kSeparator + std::wstring(word);
    if (!hwnd_) {
        return;
    }
    NOTIFYICONDATAW nid = base_data(hwnd_);
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    copy_text(nid.szTip, tooltip_);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::notify(std::wstring_view title, std::wstring_view text, Toast::Kind kind) {
    if (toast_.show({.kind = kind, .tag = L"Link", .title = std::wstring(title), .text = std::wstring(text)},
                    [this] {
                        if (actions_.open) actions_.open();
                    })) {
        return;
    }
    if (!hwnd_) {
        return;
    }
    // Fallback: the Windows notification (bottom-right).
    NOTIFYICONDATAW nid = base_data(hwnd_);
    nid.uFlags = NIF_INFO;
    copy_text(nid.szInfoTitle, title);
    copy_text(nid.szInfo, text);
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    nid.hBalloonIcon = large_icon_.get();
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::on_link_change(LinkState before, LinkState now, std::wstring_view endpoint, bool reconnects) {
    switch (now) {
    case LinkState::Waiting:
        set_state_word(L"WAITING");
        break;
    case LinkState::Connecting:
        set_state_word(L"CONNECTING");
        break;
    case LinkState::Linked:
        set_state_word(L"LINKED");
        if (before != LinkState::Linked) {
            notify(L"Connected to host", endpoint, Toast::Kind::Connected);
        }
        break;
    case LinkState::Lost:
        set_state_word(L"LOST");
        if (before != LinkState::Lost) {
            notify(L"Connection lost", reconnects ? L"reconnecting" : L"restart the client", Toast::Kind::Lost);
        }
        break;
    }
}

void TrayIcon::set_menu(std::function<std::vector<MenuItem>()> build, std::function<void(UINT)> on_command) {
    build_menu_ = std::move(build);
    on_menu_command_ = std::move(on_command);
}

namespace {

// Appends `items` to `menu`; submenus are owned by their parent menu once appended.
void fill_menu(HMENU menu, const std::vector<TrayIcon::MenuItem>& items) {
    for (const auto& item : items) {
        if (item.separator) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        const UINT state = (item.enabled ? MF_ENABLED : MF_GRAYED) | (item.checked ? MF_CHECKED : MF_UNCHECKED);
        if (!item.submenu.empty()) {
            HMENU sub = CreatePopupMenu();
            if (!sub) continue;
            fill_menu(sub, item.submenu);
            AppendMenuW(menu, MF_POPUP | state, reinterpret_cast<UINT_PTR>(sub), item.text.c_str());
            continue;
        }
        AppendMenuW(menu, MF_STRING | state, item.id, item.text.c_str());
        if (item.radio || item.bold) {
            MENUITEMINFOW info{};
            info.cbSize = sizeof(info);
            info.fMask = MIIM_FTYPE | MIIM_STATE;
            info.fType = item.radio ? MFT_RADIOCHECK : 0;
            info.fState = state | (item.bold ? MFS_DEFAULT : 0);
            SetMenuItemInfoW(menu, item.id, FALSE, &info);
        }
    }
}

}  // namespace

void TrayIcon::show_menu(int x, int y) {
    unique_menu menu(CreatePopupMenu());
    if (!menu) {
        return;
    }
    if (build_menu_) {
        fill_menu(menu.get(), build_menu_());
        SetForegroundWindow(hwnd_);  // required so the menu closes when the user clicks elsewhere
        const UINT cmd = static_cast<UINT>(
            TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, x, y, hwnd_, nullptr));
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        if (cmd != 0 && on_menu_command_) on_menu_command_(cmd);
        return;
    }
    AppendMenuW(menu.get(), MF_STRING, kMenuShowViewer, L"Show viewer");
    if (actions_.displays) {
        AppendMenuW(menu.get(), MF_STRING, kMenuDisplays, L"Display arrangement...");
    }
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, kMenuQuit, L"Quit");

    // Required so the menu closes when the user clicks elsewhere.
    SetForegroundWindow(hwnd_);
    const UINT cmd = static_cast<UINT>(
        TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, x, y, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);

    if (cmd == kMenuShowViewer && actions_.open) {
        actions_.open();
    } else if (cmd == kMenuDisplays && actions_.displays) {
        actions_.displays();
    } else if (cmd == kMenuQuit && actions_.quit) {
        actions_.quit();
    }
}

LRESULT CALLBACK TrayIcon::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<TrayIcon*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;  // CreateWindowExW has not returned yet
    }
    auto* self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle_message(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayIcon::handle_message(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kTrayCallbackMsg) {
        // NOTIFYICON_VERSION_4: LOWORD(lp) = event, wp = anchor point in screen coordinates.
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
        case NIN_BALLOONUSERCLICK:
            if (actions_.open) {
                actions_.open();
            }
            break;
        case WM_CONTEXTMENU:
            show_menu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
            break;
        default:
            break;
        }
        return 0;
    }
    if (msg == taskbar_created_msg_ && taskbar_created_msg_ != 0) {
        add_icon();  // Explorer restarted: the notification area forgot our icon
        return 0;
    }
    const HWND hwnd = hwnd_;
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace kvm
