#include "kvm/kvm_link.h"

#include <utility>
#include <variant>

#include "kvm/log.h"

namespace kvm {
namespace {

host::CaptureOptions capture_options(const KvmOptions& o) {
    host::CaptureOptions c = o.capture;
    c.client_side = o.peer_side;
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
              options_.leave_cooldown) {}

KvmLink::~KvmLink() {
    stop();
}

bool KvmLink::start() {
    if (!options_.send_input || capture_) return true;
    capture_.emplace(capture_options(options_));
    if (capture_->start()) return true;
    capture_.reset();
    log::error("kvm: input hooks failed - the other PC can still control this one, but not the other way round");
    return false;
}

void KvmLink::stop() {
    detach();
    capture_.reset();  // ~InputCapture stops the hooks
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
    inject_.release_all();
    std::lock_guard lock(sink_mutex_);
    sink_ = nullptr;
}

bool KvmLink::send(const proto::Message& msg) {
    std::lock_guard lock(sink_mutex_);
    return sink_ && sink_(msg);
}

bool KvmLink::handle(const proto::Message& msg) {
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
    if (std::holds_alternative<proto::MouseMove>(msg)) {
        if (inject_.active()) inject_.handle(msg);
        return true;  // only the KVM sends mouse_move
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
