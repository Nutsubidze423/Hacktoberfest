#pragma once
// Both directions of the KVM on one PC: this PC's mouse and keyboard can go to
// the other PC (InputCapture), and the other PC's can come here (InputInject).
// A and B run the same thing; only peer_side and is_host differ.
//
//   kvm::KvmLink kvm({.peer_side = side, .is_host = true});   // A: side = client_side, B: opposite
//   kvm.start();
//   per session:
//     kvm.attach([&](const proto::Message& m) { return conn.send_json(proto::encode(m)); });
//     for each message from the peer:  if (!kvm.handle(msg)) { ...not KVM (viewer input etc.)... }
//     kvm.detach();   // before the connection goes away
//
// Only one direction is active at a time: while the peer controls this PC, this
// PC's own input can't be handed over. If both users cross their edges at the
// same moment, the host keeps control and the client yields.
//
// Who sends what (focus_* carry y_norm, the crossing point on the shared edge):
//   controller -> controlled: focus_enter, mouse_move, mouse_button, mouse_wheel, key,
//                             focus_leave ("I took my input back": emergency, disconnect...)
//   controlled -> controller: focus_leave ("your cursor came back through my edge")

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>

#include "kvm/dpi.h"
#include "kvm/input_capture.h"
#include "kvm/input_inject.h"
#include "kvm/protocol.h"

namespace kvm {

struct KvmOptions {
    // Where the other PC stands relative to this one.
    display::Side peer_side = display::Side::Right;
    // Wins when both sides switch at the same moment (A = true, B = false).
    bool is_host = true;
    // This PC's own input can go to the peer (installs the hooks).
    bool send_input = true;
    // Hook options; client_side is replaced by peer_side.
    host::CaptureOptions capture{};
    // After the peer takes control here, its cursor can't leave again this soon.
    std::chrono::milliseconds leave_cooldown{250};
};

class KvmLink {
public:
    // Sends one message to the peer; false = the connection is gone. Thread-safe.
    using Sink = std::function<bool(const proto::Message&)>;

    explicit KvmLink(KvmOptions options);
    ~KvmLink();  // stop()

    KvmLink(const KvmLink&) = delete;
    KvmLink& operator=(const KvmLink&) = delete;

    // Installs the hooks (if send_input). false = hooks failed: this PC can still
    // be controlled by the peer, but can't control it.
    bool start();
    void stop();

    // A session with the peer starts / ends. detach() releases everything and
    // returns control here; call it before the connection goes away.
    void attach(Sink send);
    void detach();

    // KVM messages from the peer (focus_enter / focus_leave / mouse_* / key).
    // Returns false for anything that isn't KVM input right now, e.g. viewer
    // clicks and keys while the peer is not controlling this PC.
    bool handle(const proto::Message& msg);

    // Left button held at the edge towards the peer (e.g. files dragged out of
    // Explorer): show a "drop here" window at `at`. See InputCapture::set_on_drag_at_edge.
    void set_on_drag_at_edge(host::InputCapture::DragAtEdge callback);

    bool controlling_peer() const { return capture_ && capture_->remote(); }
    bool controlled_by_peer() const { return inject_.active(); }
    // Ctrl+Alt+Shift+Q was pressed here: this PC no longer sends its input.
    bool disabled() const { return capture_ && capture_->disabled(); }

private:
    bool send(const proto::Message& msg);

    KvmOptions options_;
    std::mutex sink_mutex_;
    Sink sink_;
    std::optional<host::InputCapture> capture_;
    host::InputCapture::DragAtEdge on_drag_at_edge_;
    client::InputInject inject_;
};

}  // namespace kvm
