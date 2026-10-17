#include "kvm/file_link.h"

#include <windows.h>

#include <ole2.h>
#include <shellapi.h>

#include <chrono>
#include <string>

#include "kvm/log.h"

namespace kvm {

namespace {

constexpr auto kCatcherTimeout = std::chrono::seconds(5);
constexpr unsigned long long kCatchWindowMs = 2000;  // a drop this soon after catch_drag() is that drag

std::string utf8(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// Opens Explorer on the folder that received the files, with the first item selected.
void reveal(const std::vector<std::filesystem::path>& saved) {
    if (saved.empty()) {
        return;
    }
    const std::wstring args = L"/select,\"" + saved.front().wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

}  // namespace

FileLink::FileLink()
    : drag_source_(std::make_unique<files::FileDragSource>()),
      catcher_(std::make_unique<files::FileDropCatcher>(
          [this](std::vector<std::filesystem::path> paths) { send(std::move(paths)); })) {}

FileLink::~FileLink() {
    catcher_.reset();  // stop drops before the channel goes
    set(nullptr);
}

files::Callbacks FileLink::callbacks() const {
    return {
        .on_received =
            [](const std::vector<std::filesystem::path>& saved) {
                for (const auto& p : saved) {
                    log::info("files: received {}", utf8(p));
                }
                reveal(saved);
            },
        .on_send_progress =
            [](std::uint64_t sent, std::uint64_t total) {
                if (sent == total) {
                    log::info("files: sent {:.1f} MB", total / 1e6);
                }
            },
        // The user is dragging these across: the drag goes on here from the cursor and the files
        // drop wherever the button goes up (KVM forwards the release from the other PC).
        .on_drag_received =
            [this](const std::vector<std::filesystem::path>& saved) {
                log::info("files: drag from the other PC arrived ({} item(s)), continuing it here", saved.size());
                if (!drag_source_->start(saved, [](DWORD effect) {
                        log::info("files: drag {}", effect != DROPEFFECT_NONE ? "dropped" : "cancelled");
                    })) {
                    reveal(saved);  // a drag is still running: at least show where the files are
                }
            },
    };
}

void FileLink::set(std::unique_ptr<files::FileTransfer> channel) {
    std::shared_ptr<files::FileTransfer> old;
    {
        std::lock_guard lock(mutex_);
        old = std::exchange(channel_, std::move(channel));
    }
    if (old) {
        old->close();  // joins its threads when the last reference goes, outside the lock
    }
}

void FileLink::show_catcher_at_cursor() {
    POINT pt{};
    GetCursorPos(&pt);
    catcher_->show_at(pt, kCatcherTimeout);
}

void FileLink::catch_drag(double y_norm, POINT at) {
    if (!connected()) return;  // nothing to send to: leave the drag alone
    drag_y_ = y_norm;
    drag_until_ = GetTickCount64() + kCatchWindowMs;
    catcher_->catch_now(at);  // only a drag that carries files is caught
}

void FileLink::set_on_drag_caught(std::function<void(double y_norm)> callback) {
    std::lock_guard lock(mutex_);
    on_drag_caught_ = std::move(callback);
}

bool FileLink::connected() {
    std::lock_guard lock(mutex_);
    return channel_ && channel_->is_open();
}

void FileLink::send(std::vector<std::filesystem::path> paths) {
    const bool drag = GetTickCount64() <= drag_until_.exchange(0);
    std::shared_ptr<files::FileTransfer> channel;
    std::function<void(double)> on_drag_caught;
    {
        std::lock_guard lock(mutex_);
        channel = channel_;
        on_drag_caught = on_drag_caught_;
    }
    if (!channel || !channel->is_open()) {
        log::warn("files: not connected to the other PC, {} item(s) not sent", paths.size());
        return;
    }
    for (const auto& p : paths) {
        log::info("files: {} {}", drag ? "dragging across" : "sending", utf8(p));
    }
    channel->send(std::move(paths), drag);
    if (drag && on_drag_caught) on_drag_caught(drag_y_);  // the cursor follows the files
}

}  // namespace kvm
