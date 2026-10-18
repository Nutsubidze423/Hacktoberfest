#include "kvm/file_link.h"

#include <windows.h>

#include <ole2.h>
#include <shellapi.h>

#include <chrono>
#include <optional>
#include <string>
#include <utility>

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

// A free name for `name` in `dir`: "photo.jpg", else "photo (2).jpg", "photo (3).jpg", ...
std::filesystem::path free_name(const std::filesystem::path& dir, const std::filesystem::path& name) {
    std::error_code ec;
    auto candidate = dir / name;
    for (int n = 2; std::filesystem::exists(candidate, ec) && n < 10000; ++n) {
        candidate = dir / (name.stem().wstring() + L" (" + std::to_wstring(n) + L")" + name.extension().wstring());
    }
    return candidate;
}

// A drag from the other PC that didn't drop anywhere here: its files sit in a temporary folder, so
// move them to Downloads\KVM (as a normal transfer would) and return where they ended up.
std::vector<std::filesystem::path> keep_in_downloads(const std::vector<std::filesystem::path>& items) {
    namespace fs = std::filesystem;
    const fs::path target = files::default_target();
    std::error_code ec;
    fs::create_directories(target, ec);
    std::vector<fs::path> kept;
    for (const auto& item : items) {
        if (!fs::exists(item, ec)) continue;  // already moved by a drop
        const fs::path dest = free_name(target, item.filename());
        fs::rename(item, dest, ec);
        if (ec) {  // e.g. another volume: copy, then remove the temporary copy
            ec.clear();
            fs::copy(item, dest, fs::copy_options::recursive, ec);
            if (!ec) fs::remove_all(item, ec);
        }
        if (fs::exists(dest)) {
            log::info("files: kept {}", utf8(dest));
            kept.push_back(dest);
        } else {
            log::warn("files: could not move {} to {}", utf8(item), utf8(target));
            kept.push_back(item);
        }
    }
    return kept;
}

}  // namespace

FileLink::FileLink()
    : drag_source_(std::make_unique<files::FileDragSource>()),
      catcher_(std::make_unique<files::FileDropCatcher>(
          [this](std::vector<std::filesystem::path> paths) { on_catcher_drop(std::move(paths)); })) {}

FileLink::~FileLink() {
    catcher_.reset();  // stop drops before the channel goes
    set(nullptr);
}

files::Callbacks FileLink::callbacks() const {
    return {
        .on_received =
            [this](const std::vector<std::filesystem::path>& saved) {
                for (const auto& p : saved) {
                    log::info("files: received {}", utf8(p));
                }
                ReceivedClaim claim;
                {
                    std::lock_guard lock(mutex_);
                    claim = on_received_claim_;
                }
                if (claim && claim(saved)) {
                    log::info("files: put on the clipboard, paste them with Ctrl+V");
                } else {
                    reveal(saved);
                }
            },
        // The user is dragging these across: the drag goes on here from the cursor and the files
        // drop wherever the button goes up (KVM forwards the release from the other PC).
        .on_drag_received =
            [this](const std::vector<std::filesystem::path>& saved) {
                log::info("files: drag from the other PC arrived ({} item(s)), continuing it here", saved.size());
                std::optional<files::DragImage> image;
                {
                    std::lock_guard lock(mutex_);
                    image = std::exchange(incoming_image_, std::nullopt);
                }
                drag_incoming_ = false;  // from now on the drag source itself knows whether it runs
                // If the drag can't go on here or is cancelled, the files still land in Downloads\KVM.
                if (!drag_source_->start(
                        saved,
                        [saved](DWORD effect) {
                            log::info("files: drag {}", effect != DROPEFFECT_NONE ? "dropped" : "cancelled");
                            if (effect == DROPEFFECT_NONE) reveal(keep_in_downloads(saved));
                        },
                        std::move(image))) {
                    reveal(keep_in_downloads(saved));  // a drag is still running
                }
            },
        // The drag shows the other PC's picture of these files under the cursor, as it looked there.
        .on_drag_begin =
            [this](std::optional<files::DragImage> image) {
                {
                    std::lock_guard lock(mutex_);
                    incoming_image_ = std::move(image);
                }
                drag_source_->forget_release();
                drag_incoming_ = true;
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
    if (!connected()) {
        log::info("files: a drag reached the other PC's screen but there is no file connection to send it on");
        return;  // nothing to send to: leave the drag alone
    }
    drag_y_ = y_norm;
    drag_until_ = GetTickCount64() + kCatchWindowMs;
    // Desk mode: the drag is over the other PC's screen (the virtual display). Catch it over all of
    // it, so however the cursor moves until the release it can't drop onto this PC's own desktop
    // under the virtual display; the drag then goes on over there, with its icon, to where the user
    // lets go. This PC's windows on it keep taking their drops.
    display::Rect vdd;
    bool live = false;
    if (desk_region_ && desk_region_->vdd_rect(vdd, live) && live && vdd.contains({at.x, at.y})) {
        std::vector<RECT> holes;
        for (const auto& r : desk_region_->rects()) holes.push_back(RECT{r.left, r.top, r.right, r.bottom});
        log::info("files: a drag is over the other PC's screen, catching it there to go on over there");
        catcher_->catch_over(RECT{vdd.left, vdd.top, vdd.right, vdd.bottom}, std::move(holes));
        return;
    }
    log::info("files: a drag reached the other PC's screen, trying to catch it at ({}, {})", at.x, at.y);
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
    send_now(std::move(paths), /*drag=*/false);
}

void FileLink::set_on_received_claim(ReceivedClaim claim) {
    std::lock_guard lock(mutex_);
    on_received_claim_ = std::move(claim);
}

void FileLink::on_catcher_drop(std::vector<std::filesystem::path> paths) {
    const bool drag = GetTickCount64() <= drag_until_.exchange(0);
    auto image = catcher_->take_drag_image();
    send_now(std::move(paths), drag, drag ? std::move(image) : std::nullopt);
}

void FileLink::peer_released() {
    // A running drag gets the release itself (it was injected as usual); only one that is still on
    // its way or starting needs to know where the user let go.
    if (!drag_incoming_ && !drag_source_->starting()) return;
    POINT at{};
    GetCursorPos(&at);
    log::info("files: the other PC let go at ({}, {}) before its drag ran here - it drops there", at.x, at.y);
    drag_source_->note_release(at);
}

void FileLink::send_now(std::vector<std::filesystem::path> paths, bool drag, std::optional<files::DragImage> image) {
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
    channel->send(std::move(paths), drag, std::move(image));
    if (drag && on_drag_caught) on_drag_caught(drag_y_);  // the cursor follows the files
}

}  // namespace kvm
