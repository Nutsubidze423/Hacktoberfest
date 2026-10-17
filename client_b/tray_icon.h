// Notification-area icon ("show hidden icons") with tooltip, menu and notifications (docs/DESIGN.md
// section 9). Notifications are Sarke's own card at the top-right (toast.h), not the Windows toast;
// the Windows one is only the fallback if the card can't be drawn. UI thread only.
//
//   kvm::TrayIcon tray;
//   tray.create(kvm::ui::kAppName, {.open = [&] { viewer.bring_to_front(); },
//                                   .displays = [&] { displays.show({.role = kvm::layout::Role::Client}); },
//                                   .quit = [&] { ... }});
//   viewer.set_link_listener([&](auto before, auto now) { tray.on_link_change(before, now, endpoint, true); });
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <string_view>

#include "app_logo.h"
#include "toast.h"
#include "viewer_window.h"

namespace kvm {

class TrayIcon {
public:
    struct Actions {
        std::function<void()> open;      // left click, double click, "Show viewer", clicking a notification
        std::function<void()> displays;  // "Display arrangement..." (the item shows only when set)
        std::function<void()> quit;      // "Quit"
    };

    TrayIcon() = default;
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool create(std::wstring app_name, Actions actions);

    // Tooltip "{app} · {state word}", e.g. "Sarke · LINKED".
    void set_state_word(std::wstring_view word);

    // Shows Sarke's notification card (top-right); clicking it runs Actions::open.
    void notify(std::wstring_view title, std::wstring_view text, Toast::Kind kind = Toast::Kind::Info);

    // Tooltip + notifications for a link change: "Connected to host" when the link comes up,
    // "Connection lost" when it drops. WAITING / CONNECTING never notify.
    void on_link_change(LinkState before, LinkState now, std::wstring_view endpoint, bool reconnects);

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle_message(UINT msg, WPARAM wp, LPARAM lp);
    bool add_icon();
    void show_menu(int x, int y);

    HWND hwnd_ = nullptr;
    UINT taskbar_created_msg_ = 0;
    std::wstring app_name_;
    std::wstring tooltip_;
    Actions actions_;
    ui::unique_icon small_icon_;
    ui::unique_icon large_icon_;
    Toast toast_;
};

}  // namespace kvm
