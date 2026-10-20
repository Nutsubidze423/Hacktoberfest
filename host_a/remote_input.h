// SPDX-License-Identifier: Apache-2.0

// Replays the viewer's input (from the other computer) into the shared window on the VDD.
// The viewer sends positions normalized to the stream (0..1); they land on the VDD monitor rect,
// where WindowShare keeps the window maximized. Clicks and keys go to the shared app, which is
// brought to the foreground first unless one of its own windows (e.g. a dialog) already is. If a
// click un-maximizes or moves the window, it is maximized on the VDD again (the viewer shows the
// whole VDD, so the window must keep filling it).
//
//   kvm::RemoteInput input(share->window(), share->monitor_rect());
//   // control channel thread (one thread only):
//   while (auto msg = conn->incoming().pop()) input.handle(*msg);   // ignores other messages
//   input.release_all();   // share ended / link dropped: no stuck keys or buttons
//
// Windows keys are not replayed: Win+L would lock this computer and Win+D would hide its windows.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bitset>

#include "kvm/protocol.h"

namespace kvm {

class RemoteInput {
public:
    // target: the VDD monitor rect in physical pixels (WindowShare::monitor_rect()).
    RemoteInput(HWND window, const RECT& target);
    ~RemoteInput();  // release_all()
    RemoteInput(const RemoteInput&) = delete;
    RemoteInput& operator=(const RemoteInput&) = delete;

    // Replays `msg` if it is viewer input; returns false for every other message type.
    bool handle(const proto::Message& msg);

    void handle(const proto::RemoteMouseMove& m);
    void handle(const proto::MouseButtonEvent& m);
    void handle(const proto::MouseWheel& m);
    // False if the key was not replayed (Windows keys, or Windows rejected the input).
    bool handle(const proto::Key& m);

    // Releases every key and mouse button this object pressed and hasn't released yet.
    void release_all();

    // Screen pixel for a stream position, clamped into the target rect.
    POINT to_screen(double x_norm, double y_norm) const;

private:
    bool ensure_foreground();
    void keep_parked();  // window maximized on the target again, if a click un-maximized or moved it

    HWND window_;
    DWORD window_pid_ = 0;
    RECT target_;
    std::bitset<256> keys_down_;   // by virtual-key code
    std::bitset<5> buttons_down_;  // by proto::MouseButton
};

}  // namespace kvm
