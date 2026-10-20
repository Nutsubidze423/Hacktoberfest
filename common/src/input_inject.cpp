// SPDX-License-Identifier: Apache-2.0

#include "kvm/input_inject.h"

#include <windows.h>

#include <algorithm>
#include <climits>
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

InputInject::InputInject(display::Side host_side, Sink to_host, std::chrono::milliseconds leave_cooldown)
    : host_side_(host_side), to_host_(std::move(to_host)), leave_cooldown_(leave_cooldown) {}

InputInject::~InputInject() {
    release_all();
}

bool InputInject::handle(const proto::Message& msg) {
    return std::visit(overloaded{
                          [&](const proto::FocusEnter& m) {
                              if (m.desk) {
                                  enter_desk();
                              } else {
                                  enter(m.y_norm);
                              }
                              return true;
                          },
                          [&](const proto::DeskMove& m) {
                              if (active_ && desk_) {
                                  ++received_.moves;
                                  desk_move(m.x, m.y);
                              }
                              return true;
                          },
                          [&](const proto::FocusLeave&) {
                              if (active_) {
                                  log::info("input inject: the other PC took its input back");
                                  log_received();
                              }
                              release_all();
                              return true;
                          },
                          [&](const proto::MouseMove& m) {
                              if (active_) {
                                  ++received_.moves;
                                  move(m.dx, m.dy);
                              }
                              return true;
                          },
                          [&](const proto::MouseButtonEvent& m) {
                              if (active_) {
                                  ++received_.buttons;
                                  button(m.button, m.down);
                              }
                              return true;
                          },
                          [&](const proto::MouseWheel& m) {
                              if (active_) {
                                  ++received_.wheels;
                                  check(input::send_wheel(m.delta, m.horizontal));
                              }
                              return true;
                          },
                          [&](const proto::Key& k) {
                              if (active_) {
                                  ++received_.keys;
                                  key(k);
                              }
                              return true;
                          },
                          [](const auto&) { return false; },
                      },
                      msg);
}

void InputInject::release_all() {
    for (proto::MouseButton b : buttons_down_) input::send_mouse_button(b, false);
    for (const auto& [vk, scan, extended] : keys_down_) input::send_key(vk, scan, false, extended);
    buttons_down_.clear();
    keys_down_.clear();
    const bool was_active = active_.exchange(false);
    desk_ = false;
    if (was_active) off_own_vdd();  // e.g. a drag ended on our VDD: it isn't visible here
}

std::vector<display::Monitor> InputInject::real_monitors() const {
    auto mons = display::monitors();
    if (desk_region_) {
        std::erase_if(mons, [&](const display::Monitor& m) { return desk_region_->is_vdd(m.name); });
    }
    return mons;
}

bool InputInject::off_own_vdd() {
    display::Rect vdd;
    bool live = false;
    input::Point cur;
    if (!desk_region_ || !desk_region_->vdd_rect(vdd, live) || !input::get_cursor_pos(cur) || !vdd.contains(cur)) {
        return false;
    }
    input::Point best = cur;
    long long best_d = LLONG_MAX;
    for (const auto& m : real_monitors()) {
        const auto& r = m.rect;
        const input::Point c{std::clamp(cur.x, r.left, r.right - 1), std::clamp(cur.y, r.top, r.bottom - 1)};
        const long long d = 1LL * (c.x - cur.x) * (c.x - cur.x) + 1LL * (c.y - cur.y) * (c.y - cur.y);
        if (d < best_d) {
            best_d = d;
            best = c;
        }
    }
    best = display::inward(best, host_side_, kEnterInset);
    SetCursorPos(best.x, best.y);  // not SendInput: nothing to replay through the hooks
    return true;
}

void InputInject::enter_desk() {
    primary_ = display::primary_monitor().rect;
    warned_blocked_ = false;
    received_ = {};
    entered_at_ = std::chrono::steady_clock::now();
    desk_ = true;
    active_ = true;
    log::info("input inject: the other PC's cursor is on this screen (desk mode)");
}

void InputInject::desk_move(int x, int y) {
    int px = primary_.left + x;
    int py = primary_.top + y;
    if (buttons_down_.empty()) {
        // Only the peer's virtual display maps onto our screen. While its button is held (a drag),
        // the position may go beyond, e.g. one of our windows dragged onto our own VDD.
        px = std::clamp(px, primary_.left, primary_.right - 1);
        py = std::clamp(py, primary_.top, primary_.bottom - 1);
    }
    check(input::move_cursor_abs(px, py));
}

void InputInject::set_host_side(display::Side side) {
    if (host_side_ == side) return;
    host_side_ = side;
    if (active_ && !desk_) edge_ = display::OuterEdge(real_monitors(), host_side_);  // its cursor leaves there now
}

void InputInject::enter(double y_norm) {
    edge_ = display::OuterEdge(real_monitors(), host_side_);
    const auto p = display::inward(edge_.point(y_norm), host_side_, kEnterInset);
    warned_blocked_ = false;
    received_ = {};
    entered_at_ = std::chrono::steady_clock::now();
    check(input::move_cursor_abs(p.x, p.y));
    active_ = true;
    log::info("input inject: the other PC took control, cursor at ({}, {})", p.x, p.y);
}

void InputInject::leave_to_host(double y_norm) {
    release_all();
    to_host_(proto::FocusLeave{.y_norm = y_norm});
    log::info("input inject: control back to the other PC (edge at {:.3f})", y_norm);
    log_received();
}

void InputInject::log_received() const {
    log::info("input inject: received from the other PC: {} moves, {} buttons, {} wheel, {} keys", received_.moves,
              received_.buttons, received_.wheels, received_.keys);
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
    const bool cooled_down = std::chrono::steady_clock::now() - entered_at_ >= leave_cooldown_;
    if (cooled_down && towards_host(dx, dy) && input::get_cursor_pos(cur)) {
        if (const auto t = edge_.position(cur)) {
            leave_to_host(*t);
            return;
        }
    }
    check(input::send_mouse_move_rel(dx, dy));
    // Our VDD sits on the peer's side, beyond the edge, so Windows lets the cursor through: going
    // onto it means going back to the peer.
    if (off_own_vdd() && cooled_down && input::get_cursor_pos(cur)) leave_to_host(edge_.position(cur).value_or(0.5));
}

void InputInject::button(proto::MouseButton b, bool down) {
    // A left release without our press: the peer's file drag that goes on here ends.
    const bool drag_release = !down && b == proto::MouseButton::Left && !buttons_down_.contains(b);
    if (down) {
        buttons_down_.insert(b);
    } else {
        buttons_down_.erase(b);
    }
    check(input::send_mouse_button(b, down));
    if (drag_release && on_drag_release_) on_drag_release_();
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
    log::warn(
        "input inject: Windows rejected injected input (an elevated window or the secure desktop has focus); "
        "run kvm_client as administrator to control elevated windows");
}

}  // namespace kvm::client
