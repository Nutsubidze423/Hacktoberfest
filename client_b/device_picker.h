// SPDX-License-Identifier: Apache-2.0

// "Connect to another PC" (owner: Temo): the PCs running Sarke on this network, as a Sarke card
// (docs/DESIGN.md 2 tokens: tile, radius md, Google Sans / Geist Mono) instead of a plain menu.
// Opens at the mouse cursor (the tray icon or the Displays window's Connect button), or above the tray
// when Sarke starts, and closes when it loses focus, on Esc, or after a pick.
//
//   kvm::DevicePicker picker;
//   picker.show(rows, {.connect = [&](std::size_t i) { ... }, .toggle_auto = ..., .disconnect = ...});
//   picker.update(rows);   // new scan results while it is open
//
// UI thread only. Each row: a status dot, the PC's name and IP, its state, and an "Auto" switch
// (connect to it by itself whenever it is on the network). Clicking a free row connects.
#pragma once

#include <windows.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ui_draw.h"

namespace kvm {

class DevicePicker {
public:
    struct Row {
        // Older / Newer: it runs a Sarke with another protocol version (docs/SECURITY.md 6.7).
        enum class State { Free, Busy, Connecting, Connected, Offline, Older, Newer };
        std::wstring name;
        std::wstring ip;  // empty while offline
        State state = State::Free;
        bool auto_connect = false;
        // Paired with this PC (paired.json). An unpaired PC shows "Pair…" and has no Auto switch:
        // clicking it pairs first (docs/SECURITY.md 6.3).
        bool paired = true;
    };
    struct Actions {
        std::function<void(std::size_t row)> connect;      // a free row was clicked
        std::function<void(std::size_t row)> toggle_auto;  // its Auto switch
        std::function<void()> disconnect;                  // the Disconnect button (a row is ours)
    };

    DevicePicker() = default;
    ~DevicePicker();
    DevicePicker(const DevicePicker&) = delete;
    DevicePicker& operator=(const DevicePicker&) = delete;

    // At `anchor` (screen pixels; only a click outside closes it), else at the mouse cursor.
    void show(std::vector<Row> rows, Actions actions, std::optional<POINT> anchor = std::nullopt);
    void update(std::vector<Row> rows);  // no-op while closed
    bool is_open() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_); }
    void close();

private:
    enum class Part { None, Row, Auto, Disconnect };
    struct Hit {
        Part part = Part::None;
        std::size_t row = 0;
        bool operator==(const Hit&) const = default;
    };

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle_message(UINT msg, WPARAM wp, LPARAM lp);
    bool ensure_window();
    void layout_at(POINT anchor);
    void render();
    void present();
    Hit hit_test(POINT client) const;
    bool has_ours() const;
    D2D1_RECT_F card() const;
    D2D1_RECT_F row_rect(std::size_t i) const;
    D2D1_RECT_F auto_rect(std::size_t i) const;
    D2D1_RECT_F disconnect_rect() const;
    float P(float dp) const { return static_cast<float>(ui::px(dp, dpi_)); }

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    POINT anchor_{};
    std::vector<Row> rows_;
    Actions actions_;
    Hit hover_;
    bool tracking_leave_ = false;
    // Opened at the cursor (a click on the tray icon / Connect): closes like a menu when it loses the
    // focus. Opened by itself (Sarke started): only a click outside it closes it, as the app starting
    // at the same time may take the focus.
    bool close_on_blur_ = true;
    // It was the foreground window since it opened. Windows may refuse that (another program has the
    // focus): then it can't lose focus, so a click outside it closes it.
    bool had_focus_ = false;
    ui::TextStyles text_;
    UINT text_dpi_ = 0;
    ui::LayeredSurface canvas_;
};

}  // namespace kvm
