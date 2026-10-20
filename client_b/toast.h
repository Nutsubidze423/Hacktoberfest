// SPDX-License-Identifier: Apache-2.0

// Sarke's own notification (owner: Sandro, docs/DESIGN.md 9.3): a rounded card that slides in at
// the top-right of the main screen, like NVIDIA's overlay notifications, in the website's style
// (tile, logo, "Sarke · <tag>", title, detail and a thin line that runs out until it hides) instead
// of the Windows toast at the bottom-right. Hover holds it; a click runs on_click (e.g. show the
// viewer); × closes it. Never takes focus. With animations off in Windows it only fades.
//
//   kvm::Toast toast;
//   toast.show({.kind = kvm::Toast::Kind::Connected, .tag = L"Link",
//               .title = L"Connected to tabuladze", .text = L"172.20.10.2 · 1920×1200"});
//
// UI thread only (the thread that runs the message loop).
#pragma once

#include <windows.h>

#include <d2d1.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>

#include "ui_draw.h"

namespace kvm {

class Toast {
public:
    enum class Kind { Info, Connected, Lost };

    struct Content {
        Kind kind = Kind::Info;
        std::wstring tag;    // next to the app name, e.g. L"Link"
        std::wstring title;  // one line, e.g. L"Connected to tabuladze"
        std::wstring text;   // one line, may be empty
    };

    static constexpr std::chrono::milliseconds kDefaultHold{5000};

    Toast() = default;
    ~Toast();

    Toast(const Toast&) = delete;
    Toast& operator=(const Toast&) = delete;

    // Shows the card, or swaps the content of the one already up and starts its time again.
    // False if the window can't be created (the caller may fall back to a Windows notification).
    bool show(Content content, std::function<void()> on_click = {}, std::chrono::milliseconds hold = kDefaultHold);

    // Slides it out (fades it out with animations off).
    void dismiss();

    bool visible() const { return hwnd_ != nullptr && phase_ != Phase::Hidden; }

    // Slide or not: nullopt (default) follows Windows' "Animation effects" setting.
    void set_motion(std::optional<bool> on) { motion_override_ = on; }
    HWND hwnd() const { return hwnd_; }

private:
    enum class Phase { Hidden, In, Hold, Out };
    struct Geometry {
        RECT work{};    // the main screen's work area
        UINT dpi = 96;
        int w = 0, h = 0;        // the window: card + shadow margin on every side
        int margin = 0;          // shadow margin
        int x_shown = 0, x_hidden = 0, y = 0;
    };

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle_message(UINT msg, WPARAM wp, LPARAM lp);

    bool ensure_window();
    Geometry geometry() const;
    void place(double slide, double alpha);  // slide 0 = shown, 1 = off to the right
    void tick();
    void render();
    D2D1_RECT_F card_rect() const;
    D2D1_RECT_F close_rect() const;

    HWND hwnd_ = nullptr;
    Geometry geo_;
    ui::LayeredSurface canvas_;
    ui::TextStyles text_;
    int x_ = 0;
    BYTE alpha_ = 0;

    Content content_;
    std::function<void()> on_click_;
    std::chrono::milliseconds hold_ = kDefaultHold;
    Phase phase_ = Phase::Hidden;
    std::chrono::steady_clock::time_point phase_start_{};
    std::chrono::milliseconds held_{0};  // hold time used up before the current hover
    bool hover_ = false;
    bool close_hover_ = false;
    bool tracking_leave_ = false;
    bool reduced_motion_ = false;
    std::optional<bool> motion_override_;
};

}  // namespace kvm
