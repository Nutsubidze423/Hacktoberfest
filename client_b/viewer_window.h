// Viewer window on B (docs/DESIGN.md section 4): Direct2D renderer, custom title strip,
// connection state screens, HUD. Shows A's streamed window with its aspect ratio preserved.
//
// Threads: create() and the message loop run on the UI thread. The "thread-safe" methods below
// may be called from the network / video threads; they only store data and invalidate.
#pragma once

#include <windows.h>

#include <d2d1.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "app_logo.h"
#include "jpeg_decoder.h"
#include "ui_draw.h"
#include "viewer_chrome.h"
#include "viewer_hud.h"
#include "viewer_input.h"

namespace kvm {

// Largest rect with the image's aspect ratio that fits centered inside `area`.
RECT fit_rect(int image_w, int image_h, const RECT& area);

// Connection to A as seen by B. "Streaming" / "Stalled" are derived from frames while Linked.
enum class LinkState { Waiting, Connecting, Linked, Lost };

struct FrameInfo {
    std::uint32_t seq = 0;
    // Capture time converted to B's steady_now_us() clock; nullopt while the clock offset is unknown.
    std::optional<std::int64_t> t_capture_local_us;
};

class ViewerWindow {
public:
    ViewerWindow() = default;
    ~ViewerWindow();

    ViewerWindow(const ViewerWindow&) = delete;
    ViewerWindow& operator=(const ViewerWindow&) = delete;

    // Creates the window (client 960x600 dp). A hidden viewer (visible = false) appears by itself
    // when a stream starts. Closing it gives the stream back (stream_stop, see set_close_handler).
    bool create(bool visible = true);
    HWND hwnd() const { return hwnd_; }

    // --- Thread-safe ---------------------------------------------------------------------
    void set_endpoint(const std::string& host, std::uint16_t port);
    void set_reconnects(bool reconnects);  // true: LOST says "reconnecting", else "restart the client"
    void set_link_state(LinkState state);  // leaving Linked ends the current stream
    void set_rtt_ms(std::optional<int> rtt_ms);
    void set_jpeg_quality(std::optional<int> quality);
    void start_stream(const proto::StreamStart& start);  // stream_start from the window owner
    void stop_stream();                                   // stream_stop from the window owner
    // Swaps `frame` with the pending one; on return `frame` is a spare buffer to decode into.
    // An unpainted pending frame is dropped, so the window always shows the newest frame.
    void present(BgraImage& frame, const FrameInfo& info);
    void count_payload_bytes(std::size_t bytes) { payload_bytes_ += bytes; }

    // --- UI thread -----------------------------------------------------------------------
    // Client point -> 0..1 over the drawn image. nullopt over the strip, letterbox or no frame.
    // With clamp, a point outside the visible image snaps to its edge (drags that leave the image).
    std::optional<D2D1_POINT_2F> to_normalized(POINT client, bool clamp = false) const;

    // Messages from the viewer to the window owner, called on the UI thread — wire it to
    // Connection::send_json. Input on the streamed image: remote_mouse_move, mouse_button,
    // mouse_wheel, key. Closing the viewer during a stream: stream_stop {hwnd} ("give it back").
    using MessageSink = ViewerInput::Sink;
    void set_message_sink(MessageSink sink) {
        sink_ = sink;
        input_.set_sink(std::move(sink));
    }

    // What closing the viewer does after the stream was given back. Without a handler the window
    // is destroyed and its thread gets WM_QUIT (test tools); main.cpp would rather hide it.
    void set_close_handler(std::function<void()> handler) { close_handler_ = std::move(handler); }

    // Called on the UI thread after every link state change (e.g. for the tray icon).
    using LinkListener = std::function<void(LinkState before, LinkState now)>;
    void set_link_listener(LinkListener listener) { link_listener_ = std::move(listener); }

    // Restores (if minimized) and activates the window.
    void bring_to_front();

private:
    enum class View { Waiting, Connecting, Linked, Streaming, Stalled, Lost };
    using Clock = std::chrono::steady_clock;

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle_message(UINT msg, WPARAM wp, LPARAM lp);

    void render();
    void draw_content(ui::Painter& p, View view, const D2D1_RECT_F& content);
    void draw_state_block(ui::Painter& p, View view, const D2D1_RECT_F& content);
    void draw_stalled(ui::Painter& p, const D2D1_RECT_F& content);
    bool ensure_render_target();
    void take_pending_frame();
    void on_hud_tick();
    void on_blink_tick();
    void apply_dpi(UINT dpi);
    void update_icons();
    bool input_active() const;
    void on_mouse_button(proto::MouseButton button, bool down, LPARAM lp);
    void give_back_stream();
    void resize_for_stream(int width, int height);
    void on_strip_click(viewer::StripButton button);
    LRESULT hit_test_nc(POINT screen) const;
    View current_view() const;
    bool blinking_visible(View view) const;

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;

    // Shared with other threads.
    mutable std::mutex mutex_;
    struct Shared {
        std::string host;
        std::uint16_t port = 0;
        bool reconnects = false;
        LinkState link = LinkState::Waiting;
        std::optional<int> rtt_ms;
        std::optional<int> jpeg_quality;
        std::wstring stream_title;
        std::optional<std::uint64_t> stream_hwnd;  // the owner's window while a stream_start is active
        std::uint64_t generation = 0;  // bumped when a stream ends; the UI resets per-stream state
        BgraImage pending;
        FrameInfo pending_info;
        bool has_pending = false;
    } shared_;
    std::atomic<std::uint64_t> payload_bytes_{0};

    // UI thread only.
    ui::TextStyles text_;
    ui::ComPtr<ID2D1HwndRenderTarget> rt_;
    ui::ComPtr<ID2D1SolidColorBrush> brush_;
    ui::ComPtr<ID2D1Bitmap> bitmap_;
    bool bitmap_dirty_ = false;

    BgraImage displayed_;
    bool have_frame_ = false;
    std::uint64_t seen_generation_ = 0;
    bool stream_sized_ = false;  // window already resized for this stream
    Clock::time_point last_frame_time_{};
    std::optional<std::uint32_t> last_seq_;
    std::uint64_t dropped_ = 0;
    std::optional<int> latency_ms_;
    D2D1_RECT_F image_rect_{};  // where the frame was last drawn (client px)

    std::uint64_t frames_shown_ = 0;
    std::uint64_t frames_at_last_tick_ = 0;
    std::uint64_t bytes_at_last_tick_ = 0;
    std::array<std::uint64_t, 4> frame_buckets_{};  // 4 x 250 ms = the 1 s FPS/MBPS window
    std::array<std::uint64_t, 4> byte_buckets_{};
    std::size_t bucket_ = 0;
    viewer::HudValues hud_;

    bool hud_visible_ = true;
    bool one_to_one_ = false;
    bool blink_on_ = true;
    bool reduced_motion_ = false;
    viewer::StripButton hover_ = viewer::StripButton::None;
    viewer::StripButton pressed_ = viewer::StripButton::None;
    bool tracking_mouse_ = false;
    std::wstring footer_;
    std::wstring window_text_;
    LinkListener link_listener_;
    MessageSink sink_;
    std::function<void()> close_handler_;
    ViewerInput input_;
    ui::unique_icon small_icon_;
    ui::unique_icon big_icon_;
};

}  // namespace kvm
