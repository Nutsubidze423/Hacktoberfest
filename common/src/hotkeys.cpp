// SPDX-License-Identifier: Apache-2.0

#include "kvm/hotkeys.h"

#include "kvm/log.h"

namespace kvm {

namespace {

constexpr UINT kModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;

}  // namespace

Hotkeys::~Hotkeys() {
    stop();
}

void Hotkeys::add(UINT vk, Handler handler) {
    entries_.push_back({vk, std::move(handler)});
}

bool Hotkeys::start() {
    if (thread_.joinable() || entries_.empty()) {
        return false;
    }
    std::atomic<int> registered{-1};  // -1 = thread still starting
    thread_ = std::thread([this, &registered] { run(registered); });
    registered.wait(-1);
    return registered > 0;
}

void Hotkeys::stop() {
    if (!thread_.joinable()) {
        return;
    }
    if (const DWORD id = thread_id_.load()) {
        PostThreadMessageW(id, WM_QUIT, 0, 0);
    }
    thread_.join();
}

void Hotkeys::run(std::atomic<int>& registered) {
    // Force a message queue before anyone can post WM_QUIT to this thread.
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    thread_id_ = GetCurrentThreadId();

    int count = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const UINT vk = entries_[i].vk;
        if (RegisterHotKey(nullptr, static_cast<int>(i + 1), kModifiers, vk)) {
            ++count;
        } else {
            log::warn("hotkeys: Ctrl+Alt+Shift+{} is taken by another program ({})", static_cast<char>(vk),
                      GetLastError());
        }
    }
    registered = count;
    registered.notify_all();  // `registered` lives on start()'s stack: don't touch it after this

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY && msg.wParam >= 1 && msg.wParam <= entries_.size()) {
            entries_[msg.wParam - 1].handler();
        }
    }
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        UnregisterHotKey(nullptr, static_cast<int>(i + 1));
    }
    thread_id_ = 0;
}

}  // namespace kvm
