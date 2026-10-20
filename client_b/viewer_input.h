// SPDX-License-Identifier: Apache-2.0

// Viewer input -> messages for A (B -> A): remote_mouse_move, mouse_button (with x_norm/y_norm),
// mouse_wheel, key. Positions are normalized over the drawn image (docs/DESIGN.md 4.3), never the
// letterbox. Tracks what is held so focus loss can release it on A (no stuck keys or buttons).
// UI thread only.
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "kvm/protocol.h"

namespace kvm {

struct NormPoint {
    double x = 0;
    double y = 0;
};

class ViewerInput {
public:
    // Sends one message to the window owner; false = not sent (e.g. connection closed).
    using Sink = std::function<bool(const proto::Message&)>;

    void set_sink(Sink sink) { sink_ = std::move(sink); }

    // Pointer moved to `pos` over the image. Duplicates of the last sent position are skipped.
    void mouse_move(NormPoint pos);
    // Button change at `pos`. A down is preceded by a move so A clicks exactly there.
    void mouse_button(proto::MouseButton button, bool down, NormPoint pos);
    void wheel(int delta, bool horizontal);
    // From WM_KEYDOWN/UP and WM_SYSKEYDOWN/UP: vk = wParam, lp = lParam (scan code, extended bit).
    void key(WPARAM vk, LPARAM lp, bool down);

    bool button_held(proto::MouseButton button) const;
    bool any_button_held() const;

    // Sends "up" for every held button and key (focus lost, stream ended).
    void release_all();

private:
    bool send(const proto::Message& msg);
    static std::size_t index(proto::MouseButton b) { return static_cast<std::size_t>(b); }

    struct HeldKey {
        std::uint16_t vk;
        std::uint16_t scan;
        bool extended;
    };

    Sink sink_;
    std::optional<NormPoint> last_pos_;
    std::array<bool, 5> buttons_{};  // indexed by proto::MouseButton
    std::vector<HeldKey> keys_;
};

}  // namespace kvm
