// Glue between Temo's file channel (kvm/file_transfer.h) and drop window (kvm/file_drop.h) for
// kvm_host and kvm_client: one per process, the current peer connection plugged in per session.
//
//   kvm::FileLink files;                                   // creates the "drop here" window
//   auto ch = server.accept(files.callbacks());            // A   (B: connect_files(..., files.callbacks()))
//   files.set(std::move(ch));                              // a newer connection replaces the old one
//   files.show_catcher_at_cursor();                        // from the Ctrl+Alt+Shift+F hotkey
//
// Files dropped on the window go to the peer; files from the peer land in Downloads\KVM and that
// folder opens in Explorer.
#pragma once

#include <memory>
#include <mutex>

#include "kvm/file_drop.h"
#include "kvm/file_transfer.h"

namespace kvm {

class FileLink {
public:
    FileLink();
    ~FileLink();

    FileLink(const FileLink&) = delete;
    FileLink& operator=(const FileLink&) = delete;

    // Callbacks for FileServer::accept / connect_files: log and open Explorer on arrival.
    files::Callbacks callbacks() const;

    // The connection files are sent through. nullptr closes the current one.
    void set(std::unique_ptr<files::FileTransfer> channel);

    // Shows the drop window under the mouse cursor for a few seconds.
    void show_catcher_at_cursor();

private:
    void send(std::vector<std::filesystem::path> paths);

    std::mutex mutex_;
    std::shared_ptr<files::FileTransfer> channel_;
    std::unique_ptr<files::FileDropCatcher> catcher_;
};

}  // namespace kvm
