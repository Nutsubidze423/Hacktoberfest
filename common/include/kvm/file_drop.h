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
