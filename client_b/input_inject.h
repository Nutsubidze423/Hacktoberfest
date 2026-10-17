#pragma once
// KVM on B: replays the input A sends while B has control.
//
//   kvm::client::InputInject inject(display::opposite(client_side),
//                                   [&](const proto::Message& m) { return conn.send(m); });
//   for each message from A:  inject.handle(msg);
//   on disconnect:            inject.release_all();
//
// focus_enter puts the cursor on B's edge facing A at y_norm. mouse_move is
// injected relative, so B's own pointer speed / acceleration apply. When the
// cursor already sits on the edge facing A and keeps moving towards A, B sends
// focus_leave{y_norm} back and stops. Every key and button B pressed is
// released when control leaves B (either way) or the connection drops, so
// nothing stays stuck.

#include <functional>
#include <set>
#include <tuple>
#include <vector>

#include "kvm/dpi.h"
#include "kvm/protocol.h"

namespace kvm::client {

class InputInject {
public:
    // Sends one message to A; false = the connection is gone.
    using Sink = std::function<bool(const proto::Message&)>;

    // host_side: where A stands relative to B, i.e. opposite(config client_side).
    InputInject(display::Side host_side, Sink to_host);
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

private:
    void enter(double y_norm);
    void leave_to_host(double y_norm);
    bool towards_host(int dx, int dy) const;
    void move(int dx, int dy);
    void button(proto::MouseButton b, bool down);
    void key(const proto::Key& k);
    void check(bool ok);  // warns once when Windows rejects injected input

    display::Side host_side_;
    Sink to_host_;
    display::OuterEdge edge_;
    bool active_ = false;
    bool warned_blocked_ = false;
    std::set<proto::MouseButton> buttons_down_;
    std::set<std::tuple<std::uint16_t, std::uint16_t, bool>> keys_down_;  // vk, scan, extended
};

}  // namespace kvm::client
