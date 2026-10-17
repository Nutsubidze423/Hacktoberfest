#include "viewer_input.h"

#include <algorithm>

namespace kvm {

bool ViewerInput::send(const proto::Message& msg) {
    return sink_ && sink_(msg);
}

void ViewerInput::mouse_move(NormPoint pos) {
    if (last_pos_ && last_pos_->x == pos.x && last_pos_->y == pos.y) {
        return;
    }
    if (send(proto::RemoteMouseMove{.x_norm = pos.x, .y_norm = pos.y})) {
        last_pos_ = pos;
    }
}

void ViewerInput::mouse_button(proto::MouseButton button, bool down, NormPoint pos) {
    if (down) {
        mouse_move(pos);  // the owner moves the pointer first, then presses at exactly that spot
    } else if (!buttons_[index(button)]) {
        return;  // never send an up for a down the owner did not get
    }
    const bool sent = send(proto::MouseButtonEvent{.button = button, .down = down, .x_norm = pos.x, .y_norm = pos.y});
    // An up always clears the button here; a down only counts as held if it actually went out.
    buttons_[index(button)] = down && sent;
}

void ViewerInput::wheel(int delta, bool horizontal) {
    send(proto::MouseWheel{.delta = delta, .horizontal = horizontal});
}

void ViewerInput::key(WPARAM vk, LPARAM lp, bool down) {
    const HeldKey k{.vk = static_cast<std::uint16_t>(vk),
                    .scan = static_cast<std::uint16_t>((lp >> 16) & 0xFF),
                    .extended = ((lp >> 24) & 1) != 0};
    const auto same = [&](const HeldKey& h) { return h.vk == k.vk && h.extended == k.extended; };
    const auto it = std::find_if(keys_.begin(), keys_.end(), same);
    if (!down) {
        if (it == keys_.end()) {
            return;  // its down was not sent (e.g. pressed before the viewer had focus)
        }
        keys_.erase(it);
    }
    const bool sent = send(proto::Key{.vk = k.vk, .scan = k.scan, .down = down, .extended = k.extended});
    if (down && sent && it == keys_.end()) {
        keys_.push_back(k);  // auto-repeat downs of a held key are sent but tracked once
    }
}

bool ViewerInput::button_held(proto::MouseButton button) const {
    return buttons_[index(button)];
}

bool ViewerInput::any_button_held() const {
    return std::any_of(buttons_.begin(), buttons_.end(), [](bool held) { return held; });
}

void ViewerInput::release_all() {
    const NormPoint pos = last_pos_.value_or(NormPoint{});
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        if (buttons_[i]) {
            mouse_button(static_cast<proto::MouseButton>(i), false, pos);
        }
    }
    const auto held = keys_;
    for (const HeldKey& k : held) {
        send(proto::Key{.vk = k.vk, .scan = k.scan, .down = false, .extended = k.extended});
    }
    keys_.clear();
}

}  // namespace kvm
