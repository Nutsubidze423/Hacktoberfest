// Where the other PC stands, the same on both PCs (owner: Luka). Each PC keeps, per PC it meets,
// where that one is (config.json "peers", physical: "tabuladze is on my right"), and both tell each
// other their view after the hellos (proto::Layout). The view the user set last wins on both; a tie
// (two defaults) goes to the host's. The other PC then keeps the opposite: if I say you are on my
// right, you are told that I am on your left.
//
//   auto mine = layout_sync::for_peer(cfg, hello.hostname, /*this_is_host=*/true);
//   apply(mine.side); send(layout_sync::to_message(mine));
//   on proto::Layout l:  if (auto r = layout_sync::resolve(mine, l, true); r.adopted) { save; apply(r.view.side); }
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "kvm/config.h"
#include "kvm/dpi.h"
#include "kvm/protocol.h"

namespace kvm::layout_sync {

// This PC's view: where the other PC stands, and when the user set it (unix ms; 0 = a default).
struct View {
    display::Side side = display::Side::Right;
    std::int64_t set_at = 0;
};

// The saved view for `peer_name`, else the default from client_side (which is relative to the role:
// the host keeps it as is, the client the opposite).
View for_peer(const Config& cfg, const std::string& peer_name, bool this_is_host);

// What to tell the other PC: where it stands next to this one.
proto::Layout to_message(const View& mine);

struct Result {
    View view;             // the view to use from now on
    bool adopted = false;  // the other PC's view won: save it and apply it
};
// After the other PC told us its view (`theirs.you_are` = where THIS PC stands from there). A
// malformed message keeps `mine`.
Result resolve(const View& mine, const proto::Layout& theirs, bool this_is_host);

// Unix time in milliseconds, for View::set_at.
std::int64_t now_ms();

// The place of the PC this one is connected to, for one app: applies it, saves it and says what to
// send. Thread-safe (the session thread and the launcher's command thread).
//
//   Session places(config_file, /*this_is_host=*/true, [&](Side s) { kvm.set_peer_side(s); windows.set_peer_side(s); });
//   conn.send(places.begin(hello.hostname));          // on connect
//   on proto::Layout l: places.on_message(l);         // the other PC's view
//   launcher "layout right": if (auto m = places.set_by_user(Side::Right)) conn.send(*m);
//   places.end();
class Session {
public:
    using Apply = std::function<void(display::Side)>;
    Session(std::filesystem::path config_file, bool this_is_host, Apply apply);

    // A PC connected: applies its saved place (or the default) and returns what to tell it.
    proto::Layout begin(const std::string& peer_name);
    // The other PC's view arrived: if it wins it is applied and saved. True if the place changed.
    bool on_message(const proto::Layout& theirs);
    // The user put the other PC on `side` (Displays window): applied, saved, and returned to send;
    // nullopt if no PC is connected (then it is only the default for the next one).
    std::optional<proto::Layout> set_by_user(display::Side side);
    void end();

private:
    void apply_locked(const View& view, const char* why);  // mutex_ held

    std::filesystem::path config_file_;
    bool this_is_host_;
    Apply apply_;
    std::mutex mutex_;
    std::string peer_;  // "" = not connected
    View view_;
};

}  // namespace kvm::layout_sync
