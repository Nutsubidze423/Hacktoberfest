// SPDX-License-Identifier: Apache-2.0

// File transfer channel (port 50502), the same code in both directions, over TLS 1.3 like the other
// channels. It belongs to the live control session (kvm/net.h side channels, docs/SECURITY.md 6.4).
// The host listens and the client connects; after that either side sends files and receives the
// other side's files into %USERPROFILE%\Downloads\KVM\ (folders keep their structure).
//
//   Host:   kvm::files::FileServer server(port);
//           auto pending = server.accept(me);                  // TLS done, nothing read
//           kvm::net::admit_side_channel(*pending->stream, session, "files", pending->peer, [&] {
//               link.set(std::make_unique<kvm::files::FileTransfer>(std::move(*pending), callbacks));
//           });
//   Client: auto files = kvm::files::connect_files(ip, port, me, host_id, token, callbacks);
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

#include "kvm/drag_image.h"
#include "kvm/net.h"
#include "kvm/queue.h"
#include "kvm/tls.h"

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
    // A drag transfer started arriving (receive thread); on_drag_received follows when it's complete.
    // image = the drag image the user sees on the other PC, if it came along.
    std::function<void(std::optional<DragImage> image)> on_drag_begin;
};

// %USERPROFILE%\Downloads\KVM (the user's Downloads folder, wherever it was moved to).
std::filesystem::path default_target();

// Where a peer-supplied relative path ("dir/file.txt") is saved below `root`, or nullopt if the path
// is unsafe. Exposed for tests.
std::optional<std::filesystem::path> safe_destination(const std::filesystem::path& root, std::string_view relative);

class FileTransfer {
public:
    // Takes a TLS connection whose peer has been checked and starts the send and receive threads.
    FileTransfer(net::Handshaken connection, Callbacks callbacks, std::filesystem::path target = default_target());
    ~FileTransfer();  // close(), then joins both threads
    FileTransfer(const FileTransfer&) = delete;
    FileTransfer& operator=(const FileTransfer&) = delete;

    // Queues files and folders (folders recursively); they are sent one transfer after another.
    // drag = the user is dragging them across: the peer saves them to a temp folder and continues
    // the drag there (on_drag_received). False if the channel is closed.
    // image: with a drag, the picture the shell showed under the cursor here; the drag over there
    // shows the same one (on_drag_begin).
    bool send(std::vector<std::filesystem::path> items, bool drag = false,
              std::optional<DragImage> image = std::nullopt);

    bool is_open() const { return open_; }
    const std::string& peer() const { return peer_; }
    const std::filesystem::path& target() const { return target_; }

    // Unblocks and stops both threads. Idempotent, any thread. A half-received file is deleted.
    void close();

private:
    void send_loop();
    void receive_loop();

    std::unique_ptr<tls::Stream> tls_;
    std::string peer_;
    Callbacks callbacks_;
    std::filesystem::path target_;
    std::atomic<bool> open_{true};
    struct Outgoing {
        std::vector<std::filesystem::path> items;
        bool drag = false;
        std::optional<DragImage> image;
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

    // Blocks until the peer connects and completes the TLS handshake (failures are logged and
    // skipped). nullopt after stop(). Check it with admit_side_channel() before using it.
    std::optional<net::Handshaken> accept(const tls::Credentials& me);

    // Unblocks accept() from any thread. Idempotent.
    void stop();

private:
    net::Server server_;
};

// Opens the file side channel of the live session (net::connect_side_channel). nullptr on failure
// (logged); the caller decides when to retry.
std::unique_ptr<FileTransfer> connect_files(const std::string& host, std::uint16_t port, const tls::Credentials& me,
                                            const tls::DeviceId& host_id, std::string_view token, Callbacks callbacks,
                                            std::filesystem::path target = default_target(),
                                            std::chrono::milliseconds timeout = std::chrono::seconds(2));

}  // namespace kvm::files
