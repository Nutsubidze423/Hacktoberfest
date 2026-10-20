// SPDX-License-Identifier: Apache-2.0

// Global hotkeys Ctrl+Alt+Shift+<key> for kvm_host / kvm_client (RegisterHotKey on its own thread).
//
//   kvm::Hotkeys keys;
//   keys.add('F', [&] { catcher.show_at(cursor, 5s); });
//   keys.start();   // false if none could be registered (logged); handlers run on the hotkey thread
//
// While this PC controls the other one (KVM), Demetre's hooks swallow the keys and send them to the
// peer, so the peer's hotkey fires instead. Ctrl+Alt+Shift+Q is the KVM emergency exit: never add 'Q'.
#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

namespace kvm {

class Hotkeys {
public:
    using Handler = std::function<void()>;

    Hotkeys() = default;
    ~Hotkeys();

    Hotkeys(const Hotkeys&) = delete;
    Hotkeys& operator=(const Hotkeys&) = delete;

    // Ctrl+Alt+Shift+vk. Call before start(). vk: 'A'..'Z', '0'..'9' or a VK_* code.
    void add(UINT vk, Handler handler);

    // Registers everything and starts the thread. True if at least one hotkey was registered.
    bool start();

    // Unregisters and joins. Idempotent.
    void stop();

private:
    struct Entry {
        UINT vk;
        Handler handler;
    };

    void run(std::atomic<int>& registered);

    std::vector<Entry> entries_;
    std::thread thread_;
    std::atomic<DWORD> thread_id_{0};
};

}  // namespace kvm
