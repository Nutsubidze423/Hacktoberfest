#pragma once
// Shared clipboard between the two PCs: copy on one, paste on the other.
//
//   kvm::clip::ClipboardSync clip;
//   clip.start();
//   per session:  clip.attach([&](const kvm::clip::Chunk& c) { return conn.send(...c...); });
//                 chunk from the peer -> clip.receive(chunk);
//                 clip.detach();
//
// Whenever this PC's clipboard changes, its content goes to the peer: text if
// there is any (UTF-8), otherwise an image (PNG; the app's own "PNG" format if
// it offers one, else the bitmap converted with WIC). The peer puts it on its
// clipboard as CF_UNICODETEXT, or as CF_DIB + "PNG". Content set from the peer
// is never echoed back. Files are not shared (yet).
//
// Content travels as Chunks of at most kChunkBytes (base64), so each one fits a
// control-channel message; a newer copy cancels an unsent older one.
//
// Copied files (Explorer Ctrl+C / Ctrl+X) don't travel here: this PC tells the
// peer they're coming (a "files" chunk) and hands their paths to
// on_files_copied, which sends them over the file channel. When the peer's
// file channel has them (files_received), the peer puts them on its clipboard,
// so Ctrl+V in Explorer pastes them there. Originals are never deleted.
//
//   clip.set_on_files_copied([&](std::vector<std::filesystem::path> p) { files.send(std::move(p)); });
//   file channel on_received(saved):  if (!clip.files_received(saved)) { ...dragged files: open Explorer... }

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "kvm/queue.h"

namespace kvm::clip {

// One piece of clipboard content on the wire. Pieces of the same content share
// seq; offset / total let the receiver put them together.
struct Chunk {
    std::uint32_t seq = 0;
    std::string kind;          // "text" (UTF-8), "png", or "files" (names of copied files, one per line)
    std::uint32_t total = 0;   // size of the whole content in bytes (decoded)
    std::uint32_t offset = 0;  // where this piece starts
    std::string data;          // base64 of the piece
};

// 32 KB per piece = ~43 KB of base64, well inside the 64 KB control message limit.
inline constexpr std::size_t kChunkBytes = 32 * 1024;

struct ClipboardOptions {
    bool text = true;
    bool images = true;
    bool files = true;
    // Bigger text / images are not sent (they would hold up mouse and keyboard traffic).
    std::size_t max_bytes = 16 * 1024 * 1024;
    // Copied files go along up to this size and count in total.
    std::uint64_t max_file_bytes = 4ull * 1024 * 1024 * 1024;
    std::size_t max_file_count = 20000;
};

class ClipboardSync {
public:
    // Sends one chunk to the peer; false = the connection is gone.
    using Sink = std::function<bool(const Chunk&)>;

    explicit ClipboardSync(ClipboardOptions options = {});
    ~ClipboardSync();  // stop()

    ClipboardSync(const ClipboardSync&) = delete;
    ClipboardSync& operator=(const ClipboardSync&) = delete;

    // Starts watching the clipboard. false = could not (logged).
    bool start();
    void stop();

    // A copy made while no peer was attached (or whose sending broke off) goes
    // when one is attached again, so Ctrl+V over there still pastes it.
    void attach(Sink sink);
    void detach();

    // A chunk from the peer; when the content is complete it goes on this PC's
    // clipboard. Any thread.
    void receive(const Chunk& chunk);

    // Files copied here go to this callback (clipboard thread), to be sent over
    // the file channel. Without it, copied files are not shared.
    using FilesCopied = std::function<void(std::vector<std::filesystem::path>)>;
    void set_on_files_copied(FilesCopied callback);

    // The file channel saved files from the peer (any thread). If the peer had
    // copied them, they go on this clipboard and true is returned; false =
    // they were dragged / dropped, not copied.
    bool files_received(const std::vector<std::filesystem::path>& saved);

private:
    friend struct ClipboardThread;

    struct Content {
        std::string kind;
        std::vector<std::uint8_t> bytes;
    };

    void clipboard_thread_main();
    void sender_main();
    // Clipboard thread. clip_seq = GetClipboardSequenceNumber() of what is sent.
    void send_content(const Content& content, std::uint32_t clip_seq);

    ClipboardOptions options_;
    std::atomic<void*> hwnd_{nullptr};
    std::atomic<int> started_{0};  // 0 = starting, 1 = running, -1 = failed
    std::thread clipboard_thread_;
    std::thread sender_;

    std::mutex sink_mutex_;
    Sink sink_;
    BlockingQueue<Chunk> outgoing_;
    std::atomic<std::uint32_t> next_seq_{1};
    std::atomic<std::uint32_t> latest_seq_{0};  // older unsent chunks are dropped
    // GetClipboardSequenceNumber() of the content the peer has (we sent it all,
    // or it came from the peer). A newer one goes on the next attach().
    std::atomic<std::uint32_t> synced_clip_seq_{0};
    std::atomic<std::uint64_t> sending_{0};  // (seq << 32) | clipboard sequence number being sent

    // Clipboard thread only.
    int read_tries_ = 0;   // the clipboard was busy when we wanted to read it
    int apply_tries_ = 0;  // ...or to put the peer's copy on it
    std::vector<std::filesystem::path> last_files_;  // the copied files sent last, and when
    std::chrono::steady_clock::time_point last_files_at_{};

    std::mutex incoming_mutex_;
    std::uint32_t incoming_seq_ = 0;
    std::string incoming_kind_;
    std::vector<std::uint8_t> incoming_;
    std::size_t incoming_received_ = 0;
    std::optional<Content> to_apply_;  // complete content waiting for the clipboard thread

    // Copied files: guarded by incoming_mutex_.
    FilesCopied on_files_copied_;
    std::chrono::steady_clock::time_point files_expected_until_{};  // the peer announced a file copy
    std::vector<std::filesystem::path> files_unclaimed_;            // arrived before the announcement
    std::chrono::steady_clock::time_point files_unclaimed_at_{};
    std::optional<std::vector<std::filesystem::path>> files_to_apply_;
};

// Exposed for tests.
std::string base64_encode(std::span<const std::uint8_t> bytes);
std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view text);
std::vector<Chunk> split(std::uint32_t seq, std::string_view kind, std::span<const std::uint8_t> bytes);

}  // namespace kvm::clip
