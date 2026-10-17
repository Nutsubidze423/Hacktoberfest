#include "window_link.h"

#include <chrono>
#include <cwchar>
#include <future>
#include <span>
#include <string>
#include <variant>

// Sandro's viewer lives in client_b; both exes link both module libraries.
#include "../client_b/video_receiver.h"
#include "../client_b/viewer_window.h"
#include "kvm/log.h"
#include "remote_input.h"
#include "window_picker.h"
#include "window_share.h"

namespace kvm {

namespace {

// Never sent: this process's windows (viewer, console, drop box), the desktop and the taskbar.
bool sendable(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || hwnd == GetConsoleWindow()) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return false;
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    for (const wchar_t* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"}) {
        if (std::wcscmp(cls, shell) == 0) return false;
    }
    return true;
}

std::wstring title_of(HWND hwnd) {
    std::wstring title(static_cast<size_t>(GetWindowTextLengthW(hwnd)) + 1, L'\0');
    title.resize(static_cast<size_t>(GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size()))));
    return title;
}

}  // namespace

WindowLink::WindowLink(Options options) : options_(options) {
    std::promise<void> ready;
    auto started = ready.get_future();
    ui_thread_ = std::thread([this, &ready] {
        ui_thread_id_ = GetCurrentThreadId();
        auto viewer = std::make_unique<ViewerWindow>();
        if (!viewer->create(/*visible=*/false)) log::error("windows: the viewer window could not be created");
        viewer->set_jpeg_quality(options_.jpeg_quality);
        viewer->set_reconnects(true);
        // The viewer's clicks, keys and "give it back" (stream_stop on close) go to the window's owner.
        viewer->set_message_sink([this](const proto::Message& m) { return send(m); });
        viewer->set_close_handler([this] {
            if (const HWND h = viewer_hwnd_) ShowWindow(h, SW_HIDE);
        });
        viewer_hwnd_ = viewer->hwnd();
        {
            std::lock_guard lock(mutex_);
            viewer_ = std::move(viewer);
        }
        ready.set_value();

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        std::unique_ptr<ViewerWindow> closing;
        {
            std::lock_guard lock(mutex_);
            closing = std::move(viewer_);
        }
        viewer_hwnd_ = nullptr;
        closing.reset();  // the window is destroyed on the thread that made it
    });
    started.wait();

    watchdog_ = std::jthread([this](std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::lock_guard lock(mutex_);
            if (share_ && !share_->alive()) {
                log::info("windows: the shared window was closed");
                stop_share(/*tell_peer=*/true);
            }
        }
    });
}

WindowLink::~WindowLink() {
    watchdog_.request_stop();
    if (watchdog_.joinable()) watchdog_.join();
    {
        std::lock_guard lock(mutex_);
        stop_share(/*tell_peer=*/true);  // never leave the window on an invisible monitor
    }
    set_video(nullptr);
    if (const DWORD id = ui_thread_id_) PostThreadMessageW(id, WM_QUIT, 0, 0);
    if (ui_thread_.joinable()) ui_thread_.join();
}

void WindowLink::set_video(std::shared_ptr<net::FrameChannel> channel) {
    ViewerWindow* viewer = nullptr;  // fixed for the object's life once the constructor returned
    {
        std::lock_guard lock(mutex_);
        viewer = viewer_.get();
    }
    std::shared_ptr<net::FrameChannel> old_video;  // declared first: outlives old_receiver below
    std::unique_ptr<VideoReceiver> old_receiver;
    {
        std::lock_guard lock(video_mutex_);  // never together with mutex_ (see the header)
        old_receiver = std::move(receiver_);
        old_video = std::exchange(video_, std::move(channel));
        if (video_ && viewer) receiver_ = std::make_unique<VideoReceiver>(*video_, *viewer);
    }
    old_receiver.reset();  // closes the old channel and joins its reader, outside the lock
}

void WindowLink::attach(Sink send_fn, int peer_w, int peer_h) {
    {
        std::lock_guard lock(send_mutex_);
        send_ = std::move(send_fn);
    }
    set_peer_screen(peer_w, peer_h);
    std::lock_guard lock(mutex_);
    if (viewer_) viewer_->set_link_state(LinkState::Linked);
}

void WindowLink::set_peer_screen(int peer_w, int peer_h) {
    std::lock_guard lock(mutex_);
    if (peer_w > 0 && peer_h > 0) {
        peer_w_ = peer_w;
        peer_h_ = peer_h;
    }
}

void WindowLink::detach() {
    {
        std::lock_guard lock(mutex_);
        stop_share(/*tell_peer=*/false);  // the link is gone; the window comes home anyway
        if (viewer_) {
            viewer_->stop_stream();
            viewer_->set_link_state(LinkState::Lost);
        }
    }
    std::lock_guard lock(send_mutex_);
    send_ = nullptr;
}

bool WindowLink::handle(const proto::Message& msg) {
    std::lock_guard lock(mutex_);
    return std::visit(
        proto::Overloaded{
            [this](const proto::StreamStart& s) {
                if (viewer_) viewer_->start_stream(s);  // the other PC's window, shown here
                return true;
            },
            [this](const proto::StreamStop& s) {
                if (share_ && s.hwnd == reinterpret_cast<std::uint64_t>(share_->window())) {
                    log::info("windows: the other PC gives the window back");
                    stop_share(/*tell_peer=*/false);
                } else if (viewer_) {
                    viewer_->stop_stream();
                }
                return true;
            },
            [this](const proto::RemoteMouseMove& m) {
                if (input_) input_->handle(m);
                return true;
            },
            [this, &msg](const proto::MouseButtonEvent&) { return input_ && input_->handle(msg); },
            [this, &msg](const proto::MouseWheel&) { return input_ && input_->handle(msg); },
            [this, &msg](const proto::Key&) { return input_ && input_->handle(msg); },
            [](const auto&) { return false; },
        },
        msg);
}

void WindowLink::toggle() {
    const HWND foreground = GetForegroundWindow();
    if (foreground && foreground == viewer_hwnd_.load()) {
        PostMessageW(foreground, WM_CLOSE, 0, 0);  // the viewer sends stream_stop: the window goes home
        return;
    }
    std::lock_guard lock(mutex_);
    if (share_) {
        stop_share(/*tell_peer=*/true);  // this PC's window is over there: bring it back
        return;
    }
    {
        std::lock_guard send_lock(send_mutex_);
        if (!send_) {
            log::warn("windows: not connected to the other PC");
            return;
        }
    }
    if (!sendable(foreground)) {
        log::warn("windows: click the window to send first, then press Ctrl+Alt+Shift+W");
        return;
    }
    // Frames go out on whatever video connection is current (it can be replaced on reconnect);
    // without one they are dropped and the share keeps running.
    auto send_frame = [this](const proto::FrameHeader& header, std::span<const std::uint8_t> payload) {
        std::shared_ptr<net::FrameChannel> channel;
        {
            std::lock_guard video_lock(video_mutex_);
            channel = video_;
        }
        if (channel) channel->send_frame(header, payload);
        return true;
    };
    try {
        share_ = WindowShare::start(foreground, send_frame,
                                    {.width = peer_w_,
                                     .height = peer_h_,
                                     .hz = 60,
                                     .side = options_.peer_side,
                                     .jpeg_quality = options_.jpeg_quality,
                                     .max_fps = options_.max_fps});
    } catch (const std::exception& e) {
        log::error("windows: {}", e.what());
        return;
    }
    input_ = std::make_unique<RemoteInput>(share_->window(), share_->monitor_rect());
    const std::string title = to_utf8(title_of(foreground));
    send(proto::StreamStart{.hwnd = reinterpret_cast<std::uint64_t>(foreground),
                            .title = title,
                            .width = share_->width(),
                            .height = share_->height()});
    log::info("windows: \"{}\" is on the other PC now ({}x{}); Ctrl+Alt+Shift+W brings it back", title,
              share_->width(), share_->height());
}

void WindowLink::set_rtt_ms(int rtt_ms, std::int64_t clock_offset_us) {
    {
        std::lock_guard lock(mutex_);
        if (viewer_) viewer_->set_rtt_ms(rtt_ms);
    }
    std::lock_guard lock(video_mutex_);
    if (receiver_) receiver_->set_clock_offset_us(clock_offset_us);
}

void WindowLink::stop_share(bool tell_peer) {
    if (!share_) return;
    const HWND hwnd = share_->window();
    if (input_) {
        input_->release_all();
        input_.reset();
    }
    share_.reset();  // sender, capture, window back where it was, VDD off
    if (tell_peer) send(proto::StreamStop{.hwnd = reinterpret_cast<std::uint64_t>(hwnd)});
    log::info("windows: the window is back here");
}

bool WindowLink::send(const proto::Message& msg) {
    Sink sink;
    {
        std::lock_guard lock(send_mutex_);
        sink = send_;
    }
    return sink && sink(msg);
}

}  // namespace kvm
