// SPDX-License-Identifier: Apache-2.0

#include "kvm/kvm_link.h"

#include <windows.h>

#include <algorithm>
#include <utility>
#include <variant>

#include "kvm/log.h"

namespace kvm {
namespace {

host::CaptureOptions capture_options(const KvmOptions& o) {
    host::CaptureOptions c = o.capture;
    c.client_side = o.peer_side;
    c.desk = o.desk;
    return c;
}

}  // namespace

KvmLink::KvmLink(KvmOptions options)
    : options_(std::move(options)),
      inject_(options_.peer_side,
              [this](const proto::Message& m) {
                  // Our injected cursor went back through the edge: the peer has
                  // its input again, so this PC may hand over its own.
                  if (std::holds_alternative<proto::FocusLeave>(m) && capture_) capture_->set_suspended(false);
                  return send(m);
              },
              options_.leave_cooldown) {
    inject_.set_desk(options_.desk);
}

KvmLink::~KvmLink() {
    stop();
}

bool KvmLink::start() {
    if (!options_.send_input || capture_) return true;
    capture_.emplace(capture_options(options_));
    if (on_drag_at_edge_) capture_->set_on_drag_at_edge(on_drag_at_edge_);
    if (on_emergency_) capture_->set_on_emergency(on_emergency_);
    capture_->set_on_local_activity([this] { take_back(); });
    if (capture_->start()) return true;
    capture_.reset();
    log::error("kvm: input hooks failed - the other PC can still control this one, but not the other way round");
    return false;
}

void KvmLink::stop() {
    detach();
    capture_.reset();  // ~InputCapture stops the hooks
}

void KvmLink::set_on_drag_at_edge(host::InputCapture::DragAtEdge callback) {
    on_drag_at_edge_ = std::move(callback);
    if (capture_) capture_->set_on_drag_at_edge(on_drag_at_edge_);
}

void KvmLink::switch_to_peer(double y_norm) {
    if (capture_) capture_->switch_to_remote(y_norm);
}

void KvmLink::set_on_drag_release(std::function<void()> callback) {
    std::lock_guard lock(inject_mutex_);
    inject_.set_on_drag_release(std::move(callback));
}

void KvmLink::set_on_emergency(std::function<void()> callback) {
    on_emergency_ = std::move(callback);
    if (capture_) capture_->set_on_emergency(on_emergency_);
}

void KvmLink::attach(Sink send_fn) {
    {
        std::lock_guard lock(sink_mutex_);
        sink_ = std::move(send_fn);
    }
    if (capture_) capture_->set_client([this](const proto::Message& m) { return send(m); });
}

void KvmLink::detach() {
    if (capture_) {
        capture_->set_client(nullptr);  // control back here; blocks until the capture stops sending
        capture_->set_suspended(false);
    }
    {
        std::lock_guard lock(inject_mutex_);
        inject_.release_all();
    }
    std::lock_guard lock(sink_mutex_);
    sink_ = nullptr;
}

void KvmLink::set_peer_side(display::Side side) {
    {
        std::lock_guard lock(inject_mutex_);
        if (options_.peer_side == side) return;
        options_.peer_side = side;
        inject_.set_host_side(side);
    }
    if (capture_) capture_->set_client_side(side);
    log::info("kvm: the other PC is on the {} now", display::to_string(side));
}

void KvmLink::take_back() {
    display::Side peer_side;
    {
        std::lock_guard lock(inject_mutex_);
        if (!inject_.active()) return;
        inject_.release_all();
        peer_side = options_.peer_side;
    }
    // The peer's cursor goes home where ours is now along the shared edge: the height for a peer on
    // the left or right, the x position for one above or below.
    const auto vd = display::virtual_desktop();
    POINT pt{};
    GetCursorPos(&pt);
    const bool along_x = peer_side == display::Side::Top || peer_side == display::Side::Bottom;
    const int len = along_x ? vd.width() : vd.height();
    const int at = along_x ? pt.x - vd.left : pt.y - vd.top;
    const double y_norm = len > 0 ? std::clamp((at + 0.5) / len, 0.0, 1.0) : 0.5;
    log::info("kvm: this PC's own mouse moved - taking it back from the other PC");
    send(proto::FocusLeave{.y_norm = y_norm});
    if (capture_) capture_->set_suspended(false);
}

bool KvmLink::send(const proto::Message& msg) {
    std::lock_guard lock(sink_mutex_);
    return sink_ && sink_(msg);
}

bool KvmLink::handle(const proto::Message& msg) {
    std::lock_guard lock(inject_mutex_);
    if (std::holds_alternative<proto::FocusEnter>(msg)) {
        if (controlling_peer()) {
            if (options_.is_host) {
                log::info("kvm: both PCs crossed at once - this PC (host) keeps control");
                return true;
            }
            log::info("kvm: both PCs crossed at once - the host wins, giving control back");
            capture_->switch_to_local(/*tell_peer=*/false);
        }
        if (capture_) capture_->set_suspended(true);
        inject_.handle(msg);
        return true;
    }
    if (const auto* leave = std::get_if<proto::FocusLeave>(&msg)) {
        if (controlling_peer()) {
            capture_->on_focus_leave(*leave);  // our cursor came back through the peer's edge
        } else if (inject_.active()) {
            inject_.handle(msg);  // the peer took its input back
            if (capture_) capture_->set_suspended(false);
        }
        return true;
    }
    if (std::holds_alternative<proto::MouseMove>(msg) || std::holds_alternative<proto::DeskMove>(msg)) {
        if (inject_.active()) inject_.handle(msg);
        return true;  // only the KVM sends mouse_move / desk_move
    }
    if (std::holds_alternative<proto::MouseButtonEvent>(msg) || std::holds_alternative<proto::MouseWheel>(msg) ||
        std::holds_alternative<proto::Key>(msg)) {
        if (!inject_.active()) return false;  // e.g. viewer input from B on A
        inject_.handle(msg);
        return true;
    }
    return false;
}

}  // namespace kvm
