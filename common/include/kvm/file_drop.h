// The "drop here" window at the screen edge (T9, owner: Temo). When a file drag from Explorer reaches
// the edge towards the other PC, KVM (Demetre, D1: on_drag_at_edge) calls show_at(); the user drops
// the files on it, and on_drop gets their paths, which go to FileTransfer::send().
//
//   kvm::files::FileDropCatcher catcher([&](std::vector<std::filesystem::path> paths) {
//       files->send(std::move(paths));
//   });
//   // D1 callback: catcher.show_at(kvm::display::point_on_outer_edge(mons, side, y_norm));
//
// It runs its own STA thread (OLE drag and drop needs one) with a small topmost window that never
// takes focus. show_at() and hide() only post a message, so they may be called from any thread,
// including a low-level hook callback.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace kvm::files {

class FileDropCatcher {
public:
    using OnDrop = std::function<void(std::vector<std::filesystem::path>)>;

    // on_drop runs on the catcher's thread; keep it short (FileTransfer::send only queues).
    explicit FileDropCatcher(OnDrop on_drop, std::wstring label = L"Drop here to send");
    ~FileDropCatcher();
    FileDropCatcher(const FileDropCatcher&) = delete;
    FileDropCatcher& operator=(const FileDropCatcher&) = delete;

    // Shows the window around `screen_point` (physical pixels), kept inside that monitor. It hides
    // again after `timeout` unless a drag is over it, and right after a drop.
    void show_at(POINT screen_point, std::chrono::milliseconds timeout = std::chrono::milliseconds(1500));
    void hide();

    // Cross-PC drag (T10), A side: an invisible catcher goes under `screen_point` (the cursor) and
    // the left button is released there, so the drag that is running drops onto it (on_drop gets the
    // files) while the user's hand stays on the button and KVM carries the drag to the other PC.
    void catch_now(POINT screen_point);

    // Desk mode: an invisible drop target over `area` (the virtual display = the other PC's screen,
    // physical pixels) except `holes` (this PC's own windows on it, which take their drops as usual),
    // until the left button is up and no drag is over it. A file drag the user releases over it goes
    // to `on_drop_at` with the drop point (catcher thread), instead of onto this PC's own desktop that
    // lies under the virtual display. Nothing is faked: the user's own release drops it.
    using OnDropAt = std::function<void(std::vector<std::filesystem::path>, POINT where)>;
    void cover(RECT area, std::vector<RECT> holes, OnDropAt on_drop_at);

    bool visible() const { return visible_; }

private:
    void run();

    OnDrop on_drop_;
    std::wstring label_;
    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<bool> visible_{false};
    std::thread thread_;
};

}  // namespace kvm::files
