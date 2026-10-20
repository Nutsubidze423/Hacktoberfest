// SPDX-License-Identifier: Apache-2.0

#include "kvm/layout_sync.h"

#include <chrono>
#include <exception>

#include "kvm/log.h"

namespace kvm::layout_sync {

View for_peer(const Config& cfg, const std::string& peer_name, bool this_is_host) {
    if (const auto it = cfg.peers.find(peer_name); it != cfg.peers.end()) {
        if (const auto side = display::parse_side(it->second.side)) return {*side, it->second.set_at};
    }
    const auto client_side = display::parse_side(cfg.client_side).value_or(display::Side::Right);
    return {this_is_host ? client_side : display::opposite(client_side), 0};
}

proto::Layout to_message(const View& mine) {
    return {.you_are = std::string(display::to_string(mine.side)), .set_at = mine.set_at};
}

Result resolve(const View& mine, const proto::Layout& theirs, bool this_is_host) {
    const auto me_from_there = display::parse_side(theirs.you_are);
    if (!me_from_there || theirs.set_at < 0) return {mine, false};
    const View their_view{display::opposite(*me_from_there), theirs.set_at};
    const bool theirs_wins = theirs.set_at > mine.set_at || (theirs.set_at == mine.set_at && !this_is_host);
    if (!theirs_wins) return {mine, false};
    return {their_view, their_view.side != mine.side || their_view.set_at != mine.set_at};
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Session::Session(std::filesystem::path config_file, bool this_is_host, Apply apply)
    : config_file_(std::move(config_file)), this_is_host_(this_is_host), apply_(std::move(apply)) {}

proto::Layout Session::begin(const std::string& peer_name) {
    // Read anew: the Displays window or an earlier session may have changed it since the app started.
    Config cfg;
    try {
        cfg = load_config(config_file_);
    } catch (const std::exception& e) {
        log::warn("layout: {} - using the defaults", e.what());
    }
    std::lock_guard lock(mutex_);
    peer_ = peer_name;
    view_ = for_peer(cfg, peer_name, this_is_host_);
    apply_locked(view_, view_.set_at ? "as saved for it" : "the default, not set yet");
    return to_message(view_);
}

bool Session::on_message(const proto::Layout& theirs) {
    std::lock_guard lock(mutex_);
    if (peer_.empty()) return false;
    const auto r = resolve(view_, theirs, this_is_host_);
    if (!r.adopted) return false;
    view_ = r.view;
    save_peer_layout(config_file_, peer_, {std::string(display::to_string(view_.side)), view_.set_at});
    apply_locked(view_, "as the other PC has it");
    return true;
}

std::optional<proto::Layout> Session::set_by_user(display::Side side) {
    std::lock_guard lock(mutex_);
    if (peer_.empty()) return std::nullopt;
    view_ = {side, now_ms()};
    save_peer_layout(config_file_, peer_, {std::string(display::to_string(view_.side)), view_.set_at});
    apply_locked(view_, "set on this PC");
    return to_message(view_);
}

void Session::end() {
    std::lock_guard lock(mutex_);
    peer_.clear();
}

void Session::apply_locked(const View& view, const char* why) {
    // The launcher reads this line (Displays window); keep its shape.
    log::info("layout: '{}' is on the {} of this PC ({})", peer_, display::to_string(view.side), why);
    if (apply_) apply_(view.side);
}

}  // namespace kvm::layout_sync
