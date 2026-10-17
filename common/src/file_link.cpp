#include "kvm/file_link.h"

#include <windows.h>

#include <shellapi.h>

#include <chrono>
#include <string>

#include "kvm/log.h"

namespace kvm {

namespace {

constexpr auto kCatcherTimeout = std::chrono::seconds(5);

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
    : catcher_(std::make_unique<files::FileDropCatcher>(
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

void FileLink::send(std::vector<std::filesystem::path> paths) {
    std::shared_ptr<files::FileTransfer> channel;
    {
        std::lock_guard lock(mutex_);
        channel = channel_;
    }
    if (!channel || !channel->is_open()) {
        log::warn("files: not connected to the other PC, {} item(s) not sent", paths.size());
        return;
    }
    for (const auto& p : paths) {
        log::info("files: sending {}", utf8(p));
    }
    channel->send(std::move(paths));
}

}  // namespace kvm
