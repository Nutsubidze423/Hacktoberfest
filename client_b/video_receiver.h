// B side of the video channel (port 50501): connect to A, receive JPEG frames, decode, show.
//
//   kvm::ViewerWindow viewer;  viewer.create();
//   kvm::VideoReceiver receiver(cfg.host_ip, cfg.video_port, viewer);   // runs until destroyed
//   receiver.set_clock_offset_us(conn->stats().clock_offset_us);        // optional, for latency
//
// Transport is Luka's kvm::net::FrameChannel (kvm/video.h). Threads: the receive thread reads
// frames into a LatestSlot; the decode thread takes the newest one, decodes it and presents it.
// If decoding falls behind, older frames are overwritten (dropped) instead of queueing up.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

#include "kvm/queue.h"
#include "kvm/video.h"
#include "viewer_window.h"

namespace kvm {

class VideoReceiver {
public:
    // Starts connecting right away; reconnects every second while A is not reachable.
    VideoReceiver(std::string host, std::uint16_t port, ViewerWindow& viewer);
    ~VideoReceiver();

    VideoReceiver(const VideoReceiver&) = delete;
    VideoReceiver& operator=(const VideoReceiver&) = delete;

    // A's clock minus B's clock (net::LinkStats::clock_offset_us). Until this is called the viewer
    // shows latency as unknown; 0 is exact when A and B are the same machine.
    void set_clock_offset_us(std::int64_t offset) {
        clock_offset_us_ = offset;
        has_clock_offset_ = true;
    }

private:
    void receive_loop(std::stop_token stop);
    void decode_loop(std::stop_token stop);

    std::string host_;
    std::uint16_t port_;
    ViewerWindow& viewer_;
    std::atomic<std::int64_t> clock_offset_us_{0};
    std::atomic<bool> has_clock_offset_{false};

    std::mutex channel_mutex_;  // the destructor close()s the channel to unblock recv_frame()
    std::unique_ptr<net::FrameChannel> channel_;
    LatestSlot<net::Frame> latest_;

    // Declared last: joined first on destruction, while everything above is still alive.
    std::jthread receive_thread_;
    std::jthread decode_thread_;
};

}  // namespace kvm
