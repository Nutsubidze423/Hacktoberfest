// "Connect to another PC" (owner: Temo): the PCs running Sarke on this network, as a Sarke card
// (docs/DESIGN.md 2 tokens: tile, radius md, Google Sans / Geist Mono) instead of a plain menu.
// Opens at the mouse cursor (the tray icon or the Displays window's Connect button) and closes when
// it loses focus, on Esc, or after a pick.
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
#include <string>
#include <vector>

#include "ui_draw_tmp.h"

namespace kvm {

class DevicePicker {
public:
    struct Row {
        enum class State { Free, Busy, Connecting, Connected, Offline };
        std::wstring name;
        std::wstring ip;  // empty while offline
        State state = State::Free;
        bool auto_connect = false;
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

    void show(std::vector<Row> rows, Actions actions);
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
    ui::TextStyles text_;
    UINT text_dpi_ = 0;
    ui::LayeredSurface canvas_;
};

}  // namespace kvm
