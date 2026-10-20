// SPDX-License-Identifier: Apache-2.0

// Commands the Sarke launcher writes to the app's stdin, one per line (owner: Luka):
//   layout <left|right|top|bottom>   the user moved the other PC in the Displays window
//   identify                         the user pressed Identify: the other PC shows its numbers too
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
#include "kvm/protocol.h"

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
    void set(Send send) {
        std::lock_guard lock(mutex_);
        send_ = std::move(send);
    }
    bool send(const proto::Message& msg) {
        std::lock_guard lock(mutex_);
        return send_ && send_(msg);
    }

private:
    std::mutex mutex_;
    Send send_;
};

// What both apps do with a launcher command.
void handle_launcher_command(const std::string& line, layout_sync::Session& places, CurrentPeer& peer);

}  // namespace kvm
