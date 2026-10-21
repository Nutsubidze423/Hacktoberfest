// SPDX-License-Identifier: Apache-2.0

// Commands the Sarke launcher writes to the app's stdin, one per line (owner: Luka):
//   layout <left|right|top|bottom>   the user moved the other PC in the Displays window
//   identify                         the user pressed Identify: the other PC shows its numbers too
//   pair yes | pair no               the user's answer to "pairing: code ..." (kvm/pairing.h)
//   unpair <device id>               the user unpaired that PC: forget it and end a session with it
//   share [<hwnd, hex>]              Ctrl+Alt+Shift+W for that window (default: the one in front);
//                                    handled by main.cpp, which owns the WindowLink (scripts, tests)
//
//   kvm::CurrentPeer peer;                       // a session sets / clears where messages go
//   kvm::LauncherCommands commands([&](const std::string& line) { kvm::handle_launcher_command(line, places, peer); });
//
// Only when stdin is a pipe (the launcher started the app); a console stays the user's. Declare it
// after everything its handler uses: it stops reading (and waits for the handler) when destroyed.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "kvm/layout_sync.h"
#include "kvm/paired.h"
#include "kvm/pairing.h"
#include "kvm/protocol.h"
#include "kvm/tls.h"

namespace kvm {

class LauncherCommands {
public:
    explicit LauncherCommands(std::function<void(const std::string& line)> on_line);
    ~LauncherCommands();
    LauncherCommands(const LauncherCommands&) = delete;
    LauncherCommands& operator=(const LauncherCommands&) = delete;

private:
    void run();

    std::function<void(const std::string& line)> on_line_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// The connected PC, for messages that don't come from a session (launcher commands).
class CurrentPeer {
public:
    using Send = std::function<bool(const proto::Message&)>;
    // A session starts (`send`, who it is with, how to end it) or ends (set(nullptr)).
    void set(Send send, std::optional<tls::DeviceId> id = std::nullopt, std::function<void()> end = nullptr) {
        std::lock_guard lock(mutex_);
        send_ = std::move(send);
        id_ = id;
        end_ = std::move(end);
    }
    bool send(const proto::Message& msg) {
        std::lock_guard lock(mutex_);
        return send_ && send_(msg);
    }
    // If the session is with `id`: tells it (unpaired), then ends it. False if it isn't.
    bool unpair(const tls::DeviceId& id) {
        std::lock_guard lock(mutex_);
        if (!id_ || *id_ != id) return false;
        if (send_) send_(proto::Unpaired{});
        if (end_) end_();
        return true;
    }

private:
    std::mutex mutex_;
    Send send_;
    std::optional<tls::DeviceId> id_;
    std::function<void()> end_;
};

// What both apps do with a launcher command.
void handle_launcher_command(const std::string& line, layout_sync::Session& places, CurrentPeer& peer,
                             pairing::Prompt& prompt, paired::Store& paired);

// The other PC sent `unpaired`: forgets it too. `name` is for the log line the launcher shows,
// "unpair: 'NAME' unpaired this PC".
void forget_unpaired_peer(paired::Store& paired, const tls::DeviceId& id, std::string_view name);

}  // namespace kvm
