// Desk mode, window owner (Temo): the other PC's own mouse and keyboard on our windows, which it
// draws at the same place on its screen (client_b/desk_view). Positions are normalized over our
// virtual display (VDD) and replayed there, so whatever window is under them gets the input natively.
// They aren't clamped: dragging a window from the other PC's screen back over its edge carries it
// onto our real monitors, like dragging between two monitors.
//
//   kvm::DeskInput input(region);
//   input.handle(msg);      // remote_mouse_move / mouse_button / mouse_wheel / key from the peer
//   input.release_all();    // the peer is gone, desk mode off
#pragma once

#include <set>
#include <tuple>

#include "kvm/desk_region.h"
#include "kvm/protocol.h"

namespace kvm {

class DeskInput {
public:
    explicit DeskInput(desk::Region& region) : region_(region) {}
    ~DeskInput() { release_all(); }

    DeskInput(const DeskInput&) = delete;
    DeskInput& operator=(const DeskInput&) = delete;

    // False if `msg` isn't desk input.
    bool handle(const proto::Message& msg);
    void release_all();

private:
    bool move_to(double x_norm, double y_norm);

    desk::Region& region_;
    std::set<proto::MouseButton> buttons_;
    std::set<std::tuple<std::uint16_t, std::uint16_t, bool>> keys_;  // vk, scan, extended
};

}  // namespace kvm
