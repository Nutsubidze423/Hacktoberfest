// SPDX-License-Identifier: Apache-2.0

#include "kvm/launcher_commands.h"

#include <windows.h>

#include <chrono>
#include <exception>

#include "kvm/log.h"

namespace kvm {

LauncherCommands::LauncherCommands(std::function<void(const std::string& line)> on_line)
    : on_line_(std::move(on_line)) {
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (in == nullptr || in == INVALID_HANDLE_VALUE || GetFileType(in) != FILE_TYPE_PIPE) return;
    thread_ = std::thread([this] { run(); });
}

LauncherCommands::~LauncherCommands() {
    if (!thread_.joinable()) return;
    stop_ = true;
    // The reader waits in ReadFile: cancel it (again, in case it was between two reads).
    const HANDLE h = thread_.native_handle();
    for (int i = 0; i < 50; ++i) {
        CancelSynchronousIo(h);
        if (WaitForSingleObject(h, 20) == WAIT_OBJECT_0) break;
    }
    if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
        thread_.join();
    } else {
        thread_.detach();  // stuck somewhere: the process is ending anyway
    }
}

void LauncherCommands::run() {
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    std::string pending;
    char buffer[512];
    DWORD got = 0;
    while (!stop_ && ReadFile(in, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        pending.append(buffer, got);
        for (auto end = pending.find('\n'); end != std::string::npos && !stop_; end = pending.find('\n')) {
            std::string line = pending.substr(0, end);
            pending.erase(0, end + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);  // UTF-8 BOM (.NET writers start with one)
            if (line.empty()) continue;
            try {
                on_line_(line);
            } catch (const std::exception& e) {
                log::warn("launcher command \"{}\": {}", line, e.what());
            }
        }
    }
}

void handle_launcher_command(const std::string& line, layout_sync::Session& places, CurrentPeer& peer,
                             pairing::Prompt& prompt, paired::Store& paired) {
    if (line.starts_with("layout ")) {
        const auto side = display::parse_side(line.substr(7));
        if (!side) {
            log::warn("launcher: \"{}\" - expected left, right, top or bottom", line);
            return;
        }
        if (const auto msg = places.set_by_user(*side)) {
            peer.send(*msg);  // the other PC follows: it stands on the opposite side of this one
        } else {
            log::info("layout: no PC connected - the {} is the default for the next one", display::to_string(*side));
        }
    } else if (line == "identify") {
        if (!peer.send(proto::Identify{})) log::info("identify: no PC connected");
    } else if (line == "pair yes" || line == "pair no") {
        if (!prompt.answer(line == "pair yes")) log::info("pairing: nothing to answer");
    } else if (line.starts_with("unpair ")) {
        const auto id = tls::from_hex(line.substr(7));
        if (!id) {
            log::warn("launcher: \"{}\" - expected a device ID (64 hex)", line);
            return;
        }
        paired.remove(*id);  // the launcher may have removed it already
        const bool live = peer.unpair(*id);
        log::info("unpair: {} is no longer paired with this PC{}", tls::short_id(*id),
                  live ? " - its session is closed" : "");
    } else {
        log::warn("launcher: unknown command \"{}\"", line);
    }
}

void forget_unpaired_peer(paired::Store& paired, const tls::DeviceId& id, std::string_view name) {
    paired.remove(id);
    log::warn("unpair: '{}' unpaired this PC ({}). Pair again to connect.", name, tls::short_id(id));
}

}  // namespace kvm
