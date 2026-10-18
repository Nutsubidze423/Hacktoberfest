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
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "kvm/drag_image.h"

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

    // Desk mode's catch_now(): the invisible catcher covers `area` (the virtual display = the other
    // PC's screen, physical pixels) except `holes` (this PC's own windows on it, which take their
    // drops as usual). Wherever the cursor moves before the release, it is still over the catcher, so
    // the files reach on_drop instead of falling onto this PC's desktop under the virtual display.
    void catch_over(RECT area, std::vector<RECT> holes);

    // The drag image of the last drop (the picture the shell showed under the cursor), if the drag
    // had one; it goes along so the drag shows the same picture on the other PC. Call it from on_drop.
    std::optional<DragImage> take_drag_image();

    bool visible() const { return visible_; }

private:
    void run();

    std::mutex image_mutex_;
    std::optional<DragImage> drag_image_;  // set by the drop target right before on_drop
    OnDrop on_drop_;
    std::wstring label_;
    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<bool> visible_{false};
    std::thread thread_;
};

}  // namespace kvm::files
