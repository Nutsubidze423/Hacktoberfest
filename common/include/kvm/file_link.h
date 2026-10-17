// Glue between Temo's file channel (kvm/file_transfer.h), drop window (kvm/file_drop.h) and drag
// source (kvm/file_drag.h) for kvm_host and kvm_client: one per process, the current peer
// connection plugged in per session.
//
//   kvm::FileLink files;                                   // creates the "drop here" window
//   auto ch = server.accept(files.callbacks());            // A   (B: connect_files(..., files.callbacks()))
//   files.set(std::move(ch));                              // a newer connection replaces the old one
//   files.show_catcher_at_cursor();                        // from the Ctrl+Alt+Shift+F hotkey
//
// Files dropped on the window go to the peer; files from the peer land in Downloads\KVM and that
// folder opens in Explorer.
//
// Cross-PC drag (like Mouse Without Borders): KVM reports a left-button drag at the edge
// (KvmLink::set_on_drag_at_edge) -> catch_drag(): if it carries files, they drop onto an invisible
// catcher here, go to the peer as a drag transfer, and on_drag_caught (KvmLink::switch_to_peer)
// moves the cursor over; on the peer the drag continues from the cursor and the files drop wherever
// the user lets go.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include "kvm/file_drag.h"
#include "kvm/file_drop.h"
#include "kvm/file_transfer.h"

namespace kvm {

class FileLink {
public:
    FileLink();
    ~FileLink();

    FileLink(const FileLink&) = delete;
    FileLink& operator=(const FileLink&) = delete;

    // Callbacks for FileServer::accept / connect_files: log and open Explorer on arrival, continue
    // a drag from the peer.
    files::Callbacks callbacks() const;

    // The connection files are sent through. nullptr closes the current one.
    void set(std::unique_ptr<files::FileTransfer> channel);

    // Shows the drop window under the mouse cursor for a few seconds.
    void show_catcher_at_cursor();

    // A left-button drag reached the edge towards the peer at `at` (y_norm along that edge). If it
    // carries files and the peer is connected, they are taken across (see above).
    void catch_drag(double y_norm, POINT at);

    // Called (catcher thread) once dragged files were caught and sent: hand control to the peer at
    // y_norm, e.g. [&](double y) { kvm.switch_to_peer(y); }.
    void set_on_drag_caught(std::function<void(double y_norm)> callback);

    // A normal transfer to the peer (not a drag): e.g. files copied with Ctrl+C (ClipboardSync).
    void send(std::vector<std::filesystem::path> paths);

    // Files arrived: if `claim` returns true (ClipboardSync put them on the clipboard), Explorer is
    // not opened. Call before files arrive; nullptr removes it.
    using ReceivedClaim = std::function<bool(const std::vector<std::filesystem::path>&)>;
    void set_on_received_claim(ReceivedClaim claim);

private:
    // Files dropped on the catcher window: a caught cross-PC drag, or a plain send.
    void on_catcher_drop(std::vector<std::filesystem::path> paths);
    void send_now(std::vector<std::filesystem::path> paths, bool drag);
    bool connected();

    mutable std::mutex mutex_;
    std::shared_ptr<files::FileTransfer> channel_;
    std::function<void(double)> on_drag_caught_;  // guarded by mutex_
    ReceivedClaim on_received_claim_;               // guarded by mutex_
    std::atomic<unsigned long long> drag_until_{0};  // GetTickCount64() until which a drop is a caught drag
    std::atomic<double> drag_y_{0.5};
    std::unique_ptr<files::FileDragSource> drag_source_;
    std::unique_ptr<files::FileDropCatcher> catcher_;
};

}  // namespace kvm
