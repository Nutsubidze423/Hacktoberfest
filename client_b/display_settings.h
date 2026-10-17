// Display arrangement window (owner: Sandro): "Where is the other PC?"
//
// Shows this PC's real monitors (live: re-read on WM_DISPLAYCHANGE / DPI changes), the other PC
// as its hello says (or as last seen), and the edge the cursor really crosses (the same pieces the
// KVM hook uses). The user drags the other PC to a side (or uses the buttons / arrow keys) and
// Apply writes config.json client_side for this PC's role (display_layout.h). Identify flashes each
// monitor's number on it; "Windows display settings" opens ms-settings:display.
//
//   kvm::DisplaySettings displays;
//   displays.show({.role = kvm::layout::Role::Host});      // e.g. from the tray menu, UI thread
//   displays.set_peer({.hostname = h.hostname, .width = h.screen_w, .height = h.screen_h,
//                      .scale = h.dpi_scale});             // on hello, any thread
//
// The window keeps no state the apps read: they read config.json when they start.
#pragma once

#include <windows.h>

#include <d2d1.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <string>
#include <vector>

#include "app_logo.h"
#include "display_layout.h"
#include "ui_draw_tmp.h"

namespace kvm {

class IdentifyPopup;

class DisplaySettings {
public:
    struct Options {
        std::optional<std::filesystem::path> config_path;  // default: the one kvm_host / kvm_client use
        std::optional<layout::Role> role;                  // default: guessed from host_ip, switchable
    };

    DisplaySettings();  // out of line: IdentifyPopup is only complete in the .cpp
    ~DisplaySettings();

    DisplaySettings(const DisplaySettings&) = delete;
    DisplaySettings& operator=(const DisplaySettings&) = delete;

    // Opens the window, or brings it to the front if it is open (options apply on open). UI thread.
    bool show(Options options = {});
    bool is_open() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }

    // The other PC as its hello says. Any thread. connected = it is linked right now (shown as such and
    // saved for next time with layout::remember_peer); false = only a size to draw (e.g. --peer).
    void set_peer(const layout::Peer& peer, bool connected = true);

    // Runs on the UI thread after the window is gone (closed by the user or destroyed).
    void set_close_handler(std::function<void()> on_close) { on_close_ = std::move(on_close); }

    enum class Hit {
        None, Minimize, Close, Peer,
        SideLeft, SideRight, SideTop, SideBottom,
        RoleHost, RoleClient, Apply, Revert, Identify, WindowsSettings,
    };

private:
    struct Layout {
        D2D1_RECT_F strip{}, minimize{}, close{};
        float x0 = 0, x1 = 0;
        float label_top = 0, title_top = 0, body_top = 0;
        D2D1_RECT_F map{}, panel{};
        float side_label_top = 0, side_note_top = 0, role_label_top = 0, rows_top = 0;
        D2D1_RECT_F sides[4]{};  // Left, Right, Top, Bottom
        D2D1_RECT_F roles[2]{};  // Host, Client
        D2D1_RECT_F apply{}, revert{};
        float status_top = 0;
        D2D1_RECT_F footer{}, identify{}, windows_settings{};
    };
    // Desktop pixels -> client pixels on the map.
    struct MapTransform {
        double scale = 0;
        double dx = 0, dy = 0;
        D2D1_RECT_F to_client(const display::Rect& r) const;
    };
    enum class Status { Clean, Dirty, Saved, Error };

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle_message(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT hit_test_nc(POINT screen) const;

    void apply_dpi(UINT dpi);
    void refresh_monitors();
    void reload_config();
    void set_role(layout::Role role);
    void set_pending(display::Side side);
    void apply();
    void revert();
    void identify();
    void close_identify();
    void activate(Hit hit);

    bool dirty() const;
    int peer_w() const { return peer_.width; }
    int peer_h() const { return peer_.height; }
    std::wstring peer_name() const;

    Layout compute_layout() const;
    MapTransform map_transform(const Layout& l) const;
    D2D1_RECT_F peer_box(const Layout& l, const MapTransform& t) const;
    Hit hit_test(POINT client) const;
    bool hit_enabled(Hit hit) const;

    bool ensure_render_target();
    void render();
    void draw_strip(ui::Painter& p, const Layout& l);
    void draw_header(ui::Painter& p, const Layout& l);
    void draw_map(ui::Painter& p, const Layout& l);
    void draw_panel(ui::Painter& p, const Layout& l);
    void draw_footer(ui::Painter& p, const Layout& l);
    void draw_button(ui::Painter& p, const D2D1_RECT_F& r, std::wstring_view text, Hit hit, bool primary,
                     bool selected);

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    ui::ComPtr<ID2D1HwndRenderTarget> rt_;
    ui::ComPtr<ID2D1SolidColorBrush> brush_;
    ui::TextStyles text_;
    ui::unique_icon small_icon_, big_icon_;
    std::function<void()> on_close_;

    Options options_;
    layout::Role role_ = layout::Role::Host;
    std::string computer_;
    std::vector<display::Monitor> mons_;
    layout::ConfigState cfg_;
    display::Side saved_side_ = display::Side::Right;    // the other PC's side as config.json has it
    bool saved_valid_ = true;                            // false: client_side missing-but-needed / unusable
    display::Side pending_side_ = display::Side::Right;  // what the window shows
    Status status_ = Status::Clean;
    std::wstring status_text_;                           // overrides the default hint (errors, "saved")

    mutable std::mutex peer_mutex_;                      // set_peer() may come from the network thread
    std::optional<std::pair<layout::Peer, bool>> incoming_peer_;  // peer, connected
    layout::Peer peer_;
    bool peer_live_ = false;                             // set_peer(.., connected = true)
    bool peer_set_ = false;                              // set_peer() was called: ignore peer.json

    Hit hover_ = Hit::None;
    Hit pressed_ = Hit::None;
    bool tracking_leave_ = false;
    bool dragging_ = false;
    bool drag_moved_ = false;
    POINT drag_start_{};
    D2D1_POINT_2F drag_grab_{};                          // pointer offset inside the peer box
    D2D1_RECT_F drag_box_{};
    display::Side drag_side_ = display::Side::Right;

    std::vector<std::unique_ptr<IdentifyPopup>> identify_;  // Identify: one per monitor
};

}  // namespace kvm
