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

#include <atomic>
#include <cstddef>
#include <cstdint>
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
    std::string kind;          // "text" (UTF-8) or "png"
    std::uint32_t total = 0;   // size of the whole content in bytes (decoded)
    std::uint32_t offset = 0;  // where this piece starts
    std::string data;          // base64 of the piece
};

// 32 KB per piece = ~43 KB of base64, well inside the 64 KB control message limit.
inline constexpr std::size_t kChunkBytes = 32 * 1024;

struct ClipboardOptions {
    bool text = true;
    bool images = true;
    // Bigger content is not sent (it would hold up mouse and keyboard traffic).
    std::size_t max_bytes = 16 * 1024 * 1024;
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

    void attach(Sink sink);
    void detach();

    // A chunk from the peer; when the content is complete it goes on this PC's
    // clipboard. Any thread.
    void receive(const Chunk& chunk);

private:
    friend struct ClipboardThread;

    struct Content {
        std::string kind;
        std::vector<std::uint8_t> bytes;
    };

    void clipboard_thread_main();
    void sender_main();
    void send_content(const Content& content);  // clipboard thread

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

    std::mutex incoming_mutex_;
    std::uint32_t incoming_seq_ = 0;
    std::string incoming_kind_;
    std::vector<std::uint8_t> incoming_;
    std::size_t incoming_received_ = 0;
    std::optional<Content> to_apply_;  // complete content waiting for the clipboard thread
};

// Exposed for tests.
std::string base64_encode(std::span<const std::uint8_t> bytes);
std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view text);
std::vector<Chunk> split(std::uint32_t seq, std::string_view kind, std::span<const std::uint8_t> bytes);

}  // namespace kvm::clip
