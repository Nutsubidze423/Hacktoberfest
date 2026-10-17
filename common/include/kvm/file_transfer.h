// File transfer channel (port 50502), the same code in both directions (T9, owner: Temo).
// A listens and B connects, like the other channels; after that either side sends files and
// receives the other side's files into %USERPROFILE%\Downloads\KVM\ (folders keep their structure).
//
//   A:  kvm::files::FileServer server(kvm::files::kFilePort);
//       auto files = server.accept(callbacks);                 // blocks; nullptr after stop()
//   B:  auto files = kvm::files::connect_files(ip, kvm::files::kFilePort, callbacks);
//   files->send({L"C:\\photos\\cat.jpg", L"C:\\work\\project"});   // returns at once
//
// Wire: [u32 header_len][u32 payload_len][header JSON][payload], big-endian, like the video channel.
//   {"type":"file_begin","id":1,"path":"project/src/main.cpp","size":1234}
//   {"type":"file_chunk","id":1,"offset":0}  + up to 1 MB of file data
//   {"type":"file_end","id":1}
//   {"type":"transfer_end","count":3}
// A drag transfer starts with {"type":"transfer_begin","drag":true}; its files go to a temp folder.
// Paths from the peer are checked (no "..", absolute paths, drive letters, "\\?\", ":" or device
// names like CON) and everything is written below the target folder. An existing name gets " (2)".
#pragma once

#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "kvm/queue.h"

namespace kvm::files {

inline constexpr std::uint16_t kFilePort = 50502;
inline constexpr std::size_t kChunkBytes = 1024 * 1024;

struct Callbacks {
    // A whole transfer arrived (receive thread): the top-level files and folders as saved.
    std::function<void(const std::vector<std::filesystem::path>& saved)> on_received;
    // Progress of the transfer being sent (send thread). The last call has sent == total.
    std::function<void(std::uint64_t sent_bytes, std::uint64_t total_bytes)> on_send_progress;
    // A drag transfer (send(..., true)) arrived into a temp folder: continue the drag here with
    // FileDragSource. Without this callback drag transfers go to on_received like any other.
    std::function<void(const std::vector<std::filesystem::path>& saved)> on_drag_received;
};

// %USERPROFILE%\Downloads\KVM (the user's Downloads folder, wherever it was moved to).
std::filesystem::path default_target();

// Where a peer-supplied relative path ("dir/file.txt") is saved below `root`, or nullopt if the path
// is unsafe. Exposed for tests.
std::optional<std::filesystem::path> safe_destination(const std::filesystem::path& root, std::string_view relative);

class FileTransfer {
public:
    // Takes ownership of a connected socket and starts the send and receive threads.
    FileTransfer(SOCKET sock, std::string peer, Callbacks callbacks, std::filesystem::path target = default_target());
    ~FileTransfer();  // close(), then joins both threads
    FileTransfer(const FileTransfer&) = delete;
    FileTransfer& operator=(const FileTransfer&) = delete;

    // Queues files and folders (folders recursively); they are sent one transfer after another.
    // drag = the user is dragging them across: the peer saves them to a temp folder and continues
    // the drag there (on_drag_received). False if the channel is closed.
    bool send(std::vector<std::filesystem::path> items, bool drag = false);

    bool is_open() const { return open_; }
    const std::string& peer() const { return peer_; }
    const std::filesystem::path& target() const { return target_; }

    // Unblocks and stops both threads. Idempotent, any thread. A half-received file is deleted.
    void close();

private:
    void send_loop();
    void receive_loop();

    SOCKET sock_;
    std::string peer_;
    Callbacks callbacks_;
    std::filesystem::path target_;
    std::atomic<bool> open_{true};
    struct Outgoing {
        std::vector<std::filesystem::path> items;
        bool drag = false;
    };
    BlockingQueue<Outgoing> outgoing_;
    std::thread receiver_;
    std::thread sender_;
};

class FileServer {
public:
    // Listens on 0.0.0.0:port. Throws std::runtime_error if the port cannot be bound.
    explicit FileServer(std::uint16_t port = kFilePort);
    ~FileServer();
    FileServer(const FileServer&) = delete;
    FileServer& operator=(const FileServer&) = delete;

    // Blocks until the peer connects. nullptr after stop().
    std::unique_ptr<FileTransfer> accept(Callbacks callbacks, std::filesystem::path target = default_target());

    // Unblocks accept() from any thread. Idempotent.
    void stop();

private:
    std::atomic<SOCKET> listen_sock_{INVALID_SOCKET};
};

// Connects within `timeout`. nullptr on failure (logged); the caller decides when to retry.
std::unique_ptr<FileTransfer> connect_files(const std::string& host, std::uint16_t port, Callbacks callbacks,
                                            std::filesystem::path target = default_target(),
                                            std::chrono::milliseconds timeout = std::chrono::seconds(2));

}  // namespace kvm::files
