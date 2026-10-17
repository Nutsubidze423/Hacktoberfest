#include "input_inject.h"

#include <utility>
#include <variant>

#include "kvm/log.h"
#include "kvm/win32_input.h"

namespace kvm::client {
namespace {

// The cursor enters this far inside B's edge, so the first movement towards A
// does not bounce it straight back.
constexpr int kEnterInset = 2;

template <class... F>
struct overloaded : F... {
    using F::operator()...;
};

}  // namespace

InputInject::InputInject(display::Side host_side, Sink to_host)
    : host_side_(host_side), to_host_(std::move(to_host)) {}

InputInject::~InputInject() {
    release_all();
}

bool InputInject::handle(const proto::Message& msg) {
    return std::visit(
        overloaded{
            [&](const proto::FocusEnter& m) { enter(m.y_norm); return true; },
            [&](const proto::FocusLeave&) {
                if (active_) log::info("input inject: control -> A (taken back by A)");
                release_all();
                return true;
            },
            [&](const proto::MouseMove& m) { if (active_) move(m.dx, m.dy); return true; },
            [&](const proto::MouseButtonEvent& m) { if (active_) button(m.button, m.down); return true; },
            [&](const proto::MouseWheel& m) { if (active_) check(input::send_wheel(m.delta, m.horizontal)); return true; },
            [&](const proto::Key& k) { if (active_) key(k); return true; },
            [](const auto&) { return false; },
        },
        msg);
}

void InputInject::release_all() {
    for (proto::MouseButton b : buttons_down_) input::send_mouse_button(b, false);
    for (const auto& [vk, scan, extended] : keys_down_) input::send_key(vk, scan, false, extended);
    buttons_down_.clear();
    keys_down_.clear();
    active_ = false;
}

void InputInject::enter(double y_norm) {
    edge_ = display::OuterEdge(display::monitors(), host_side_);
    const auto p = display::inward(edge_.point(y_norm), host_side_, kEnterInset);
    warned_blocked_ = false;
    check(input::move_cursor_abs(p.x, p.y));
    active_ = true;
    log::info("input inject: control -> B, cursor at ({}, {})", p.x, p.y);
}

void InputInject::leave_to_host(double y_norm) {
    release_all();
    to_host_(proto::FocusLeave{.y_norm = y_norm});
    log::info("input inject: control -> A (edge at {:.3f})", y_norm);
}

bool InputInject::towards_host(int dx, int dy) const {
    switch (host_side_) {
        case display::Side::Left: return dx < 0;
        case display::Side::Right: return dx > 0;
        case display::Side::Top: return dy < 0;
        default: return dy > 0;
    }
}

void InputInject::move(int dx, int dy) {
    // Hand control back once the cursor already sits on the edge facing A and
    // the mouse keeps pushing that way.
    input::Point cur;
    if (towards_host(dx, dy) && input::get_cursor_pos(cur)) {
        if (const auto t = edge_.position(cur)) {
            leave_to_host(*t);
            return;
        }
    }
    check(input::send_mouse_move_rel(dx, dy));
}

void InputInject::button(proto::MouseButton b, bool down) {
    if (down) {
        buttons_down_.insert(b);
    } else {
        buttons_down_.erase(b);
    }
    check(input::send_mouse_button(b, down));
}

void InputInject::key(const proto::Key& k) {
    const auto id = std::tuple{k.vk, k.scan, k.extended};
    if (k.down) {
        keys_down_.insert(id);
    } else {
        keys_down_.erase(id);
    }
    check(input::send_key(k.vk, k.scan, k.down, k.extended));
}

void InputInject::check(bool ok) {
    if (ok || warned_blocked_) return;
    warned_blocked_ = true;
    log::warn("input inject: Windows rejected injected input (an elevated window or the secure desktop has focus); "
              "run kvm_client as administrator to control elevated windows");
}

}  // namespace kvm::client
