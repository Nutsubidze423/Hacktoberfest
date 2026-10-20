// SPDX-License-Identifier: Apache-2.0

#pragma once
// KVM, receiving side: replays the input the other PC ("the peer") sends while
// it controls this PC. Runs on B and, for B -> A control, on A too. Usually
// driven by kvm::KvmLink (kvm_link.h).
//
//   kvm::client::InputInject inject(peer_side,
//                                   [&](const proto::Message& m) { return conn.send(m); });
//   for each message from the peer:  inject.handle(msg);
//   on disconnect:                   inject.release_all();
//
// focus_enter puts the cursor on this PC's edge facing the peer at y_norm.
// mouse_move is injected relative, so this PC's pointer speed / acceleration
// apply. When the cursor already sits on that edge and keeps moving towards the
// peer, focus_leave{y_norm} goes back and injection stops. Every key and button
// pressed here is released when control leaves (either way) or the connection
// drops, so nothing stays stuck.
//
// Desk mode (focus_enter{desk}): the cursor isn't moved on enter; desk_move puts
// it exactly where the peer's cursor is on its virtual display, which is the size
// of this PC's primary monitor. The peer decides when it leaves. If this PC has a
// VDD of its own (set_desk), the cursor never gets lost on it.

#include <atomic>
#include <chrono>
#include <functional>
#include <set>
#include <tuple>
#include <vector>

#include "kvm/desk_region.h"
#include "kvm/dpi.h"
#include "kvm/protocol.h"

namespace kvm::client {

class InputInject {
public:
    // Sends one message to the peer; false = the connection is gone.
    using Sink = std::function<bool(const proto::Message&)>;

    // host_side: where the peer stands relative to this PC (on B that is
    // opposite(config client_side), on A client_side itself).
    // leave_cooldown: after focus_enter, the edge facing the peer can't hand
    // control back for this long, so a jittery hand doesn't bounce straight back.
    InputInject(display::Side host_side, Sink to_host,
                std::chrono::milliseconds leave_cooldown = std::chrono::milliseconds(250));
    ~InputInject();  // release_all()

    InputInject(const InputInject&) = delete;
    InputInject& operator=(const InputInject&) = delete;

    // Handles focus_enter / focus_leave / mouse_move / mouse_button / mouse_wheel
    // / key. Returns false for any other message type (not ours).
    bool handle(const proto::Message& msg);

    // B has control right now.
    bool active() const { return active_; }

    // Releases held keys/buttons and stops. For disconnects.
    void release_all();

    // This PC's own desk Region (its VDD, lent to the peer); nullptr = none. Must outlive this.
    void set_desk(const desk::Region* region) { desk_region_ = region; }

    // The peer stands on another side now. Same thread as handle() (KvmLink holds its lock).
    void set_host_side(display::Side side);

    // The peer let go of a left button it didn't press here: the end of a file drag it carried over
    // (FileLink continues it here). Runs on the thread that calls handle(); keep it short.
    void set_on_drag_release(std::function<void()> callback) { on_drag_release_ = std::move(callback); }

private:
    void enter(double y_norm);
    void enter_desk();
    void desk_move(int x, int y);
    std::vector<display::Monitor> real_monitors() const;  // without our own VDD
    bool off_own_vdd();  // moves the cursor off our VDD if it is there; true if it was
    void leave_to_host(double y_norm);
    bool towards_host(int dx, int dy) const;
    void move(int dx, int dy);
    void button(proto::MouseButton b, bool down);
    void key(const proto::Key& k);
    void check(bool ok);  // warns once when Windows rejects injected input
    void log_received() const;

    display::Side host_side_;
    Sink to_host_;
    std::chrono::milliseconds leave_cooldown_;
    std::chrono::steady_clock::time_point entered_at_{};
    display::OuterEdge edge_;
    const desk::Region* desk_region_ = nullptr;
    bool desk_ = false;           // this stay is desk mode
    display::Rect primary_{};     // desk mode: where desk_move positions land
    std::atomic<bool> active_{false};  // read from other threads (KvmLink::controlled_by_peer)
    bool warned_blocked_ = false;
    struct {
        int moves = 0, buttons = 0, wheels = 0, keys = 0;
    } received_;  // per stay on B, for the log
    std::set<proto::MouseButton> buttons_down_;
    std::set<std::tuple<std::uint16_t, std::uint16_t, bool>> keys_down_;  // vk, scan, extended
    std::function<void()> on_drag_release_;
};

}  // namespace kvm::client
