// SPDX-License-Identifier: Apache-2.0

// "Paired PCs" (roadmap P1.9, docs/SECURITY.md 6.6): the PCs this one is paired with, as a Sarke card
// like the device picker (docs/DESIGN.md: tile, radius md, Google Sans / Geist Mono). Opens at the
// mouse cursor (the tray menu) and closes when it loses focus or on Esc.
//
//   kvm::PairedWindow window;
//   window.show(rows, {.toggle_auto = [&](std::size_t i) { ... }, .unpair = [&](std::size_t i) { ... }});
//   window.update(rows);   // after a change, while it is open
//
// UI thread only. Each row: the PC's name, its short device ID (what both PCs showed when they
// paired), when it was last seen, an "Auto" switch and an Unpair button. `unpair` may show a modal
// dialog: the card stays open meanwhile.
#pragma once

#include <windows.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ui_draw.h"

namespace kvm {

class PairedWindow {
public:
    struct Row {
        std::wstring name;
        std::wstring fingerprint;  // tls::short_id, e.g. 1A2B-3C4D-5E6F-7A8B
        std::wstring seen;         // "Connected", "Seen 5 min ago", ...
        bool connected = false;
        bool auto_connect = false;
    };
    struct Actions {
        std::function<void(std::size_t row)> toggle_auto;
        std::function<void(std::size_t row)> unpair;
    };

    PairedWindow() = default;
    ~PairedWindow();
    PairedWindow(const PairedWindow&) = delete;
    PairedWindow& operator=(const PairedWindow&) = delete;

    void show(std::vector<Row> rows, Actions actions);  // at the mouse cursor
    void update(std::vector<Row> rows);                 // no-op while closed
    bool is_open() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_); }
    void close();
    HWND hwnd() const { return hwnd_; }  // the owner for a dialog an action shows

private:
    enum class Part { None, Auto, Unpair };
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
    void take_focus();
    Hit hit_test(POINT client) const;
    D2D1_RECT_F card() const;
    D2D1_RECT_F row_rect(std::size_t i) const;
    D2D1_RECT_F unpair_rect(std::size_t i) const;
    D2D1_RECT_F auto_rect(std::size_t i) const;
    float P(float dp) const { return static_cast<float>(ui::px(dp, dpi_)); }

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    POINT anchor_{};
    std::vector<Row> rows_;
    Actions actions_;
    Hit hover_;
    bool tracking_leave_ = false;
    bool had_focus_ = false;  // it was the foreground window: losing the focus closes it
    bool in_action_ = false;  // an action's dialog is up: losing the focus to it doesn't close the card
    ui::TextStyles text_;
    UINT text_dpi_ = 0;
    ui::LayeredSurface canvas_;
};

}  // namespace kvm
