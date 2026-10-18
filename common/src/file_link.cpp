#include "kvm/file_link.h"

#include <windows.h>

// OLE first: WIN32_LEAN_AND_MEAN leaves it out of windows.h, and exdisp.h needs it.
#include <ole2.h>
#include <oleauto.h>
#include <ocidl.h>
#include <exdisp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <shellapi.h>
#include <dwmapi.h>

#include <chrono>
#include <optional>
#include <string>
#include <thread>

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

// Moves received items (in a temporary folder) into `target` under free names and returns where
// they ended up; an item that can't be moved stays where it is (and is returned as is).
std::vector<std::filesystem::path> move_into(const std::vector<std::filesystem::path>& items,
                                             const std::filesystem::path& target, bool* all_moved = nullptr) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(target, ec);
    std::vector<fs::path> moved;
    if (all_moved) *all_moved = true;
    for (const auto& item : items) {
        if (!fs::exists(item, ec)) continue;  // already moved by a drop
        const fs::path dest = free_name(target, item.filename());
        ec.clear();
        fs::rename(item, dest, ec);
        if (ec) {  // e.g. another volume: copy, then remove the temporary copy
            ec.clear();
            fs::copy(item, dest, fs::copy_options::recursive, ec);
            if (!ec) fs::remove_all(item, ec);
        }
        if (fs::exists(dest)) {
            log::info("files: put {}", utf8(dest));
            moved.push_back(dest);
        } else {
            log::warn("files: could not move {} to {}", utf8(item), utf8(target));
            moved.push_back(item);
            if (all_moved) *all_moved = false;
        }
    }
    return moved;
}

// A drag from the other PC that didn't drop anywhere here: its files sit in a temporary folder, so
// move them to Downloads\KVM (as a normal transfer would) and return where they ended up.
std::vector<std::filesystem::path> keep_in_downloads(const std::vector<std::filesystem::path>& items) {
    return move_into(items, files::default_target());
}

std::optional<std::filesystem::path> known_folder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::optional<std::filesystem::path> out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) out = std::filesystem::path(path);
    CoTaskMemFree(path);
    return out;
}

// The current folder of the Explorer window `top` (its active tab on Windows 11). Needs COM (STA).
std::optional<std::filesystem::path> explorer_folder(HWND top) {
    HWND tab = nullptr;  // Windows 11 tabs: the visible ShellTabWindowClass child is the active tab
    for (HWND t = FindWindowExW(top, nullptr, L"ShellTabWindowClass", nullptr); t;
         t = FindWindowExW(top, t, L"ShellTabWindowClass", nullptr)) {
        if (IsWindowVisible(t)) {
            tab = t;
            break;
        }
    }
    IShellWindows* windows = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) return std::nullopt;
    std::optional<std::filesystem::path> found;
    long count = 0;
    windows->get_Count(&count);
    for (long i = 0; i < count && !found; ++i) {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;
        IDispatch* disp = nullptr;
        if (FAILED(windows->Item(index, &disp)) || !disp) continue;
        IWebBrowserApp* app = nullptr;
        SHANDLE_PTR hwnd = 0;
        if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&app))) && SUCCEEDED(app->get_HWND(&hwnd)) &&
            reinterpret_cast<HWND>(hwnd) == top) {
            IServiceProvider* services = nullptr;
            IShellBrowser* browser = nullptr;
            if (SUCCEEDED(app->QueryInterface(IID_PPV_ARGS(&services))) &&
                SUCCEEDED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))) {
                HWND browser_window = nullptr;
                browser->GetWindow(&browser_window);
                IShellView* view = nullptr;
                if ((!tab || browser_window == tab) && SUCCEEDED(browser->QueryActiveShellView(&view))) {
                    IFolderView* folder_view = nullptr;
                    IPersistFolder2* folder = nullptr;
                    PIDLIST_ABSOLUTE pidl = nullptr;
                    wchar_t path[MAX_PATH]{};
                    if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&folder_view))) &&
                        SUCCEEDED(folder_view->GetFolder(IID_PPV_ARGS(&folder))) && SUCCEEDED(folder->GetCurFolder(&pidl)) &&
                        SHGetPathFromIDListW(pidl, path)) {
                        found = std::filesystem::path(path);  // a real folder; "This PC" and the like have none
                    }
                    if (pidl) CoTaskMemFree(pidl);
                    if (folder) folder->Release();
                    if (folder_view) folder_view->Release();
                    view->Release();
                }
                browser->Release();
            }
            if (services) services->Release();
        }
        if (app) app->Release();
        disp->Release();
    }
    windows->Release();
    return found;
}

// Where files dropped at `pt` (this PC's screen) belong: the folder of an Explorer window there, or
// the desktop; nullopt if some other app's window is there. Needs COM (STA).
std::optional<std::filesystem::path> folder_at(POINT pt, std::wstring& what) {
    struct Ctx {
        POINT pt;
        HWND hit = nullptr;
    } ctx{pt};
    // Top-level windows in z-order, topmost first: the first real one under the point is what the
    // user dropped onto. Overlays that let clicks through (our desk view) and our own windows don't count.
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto& c = *reinterpret_cast<Ctx*>(lp);
            if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
            const auto ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
            if ((ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT)) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == GetCurrentProcessId()) return TRUE;
            BOOL cloaked = FALSE;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (cloaked) return TRUE;
            RECT r{};
            if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) && !GetWindowRect(h, &r)) {
                return TRUE;
            }
            if (!PtInRect(&r, c.pt)) return TRUE;
            c.hit = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    wchar_t cls[64]{};
    if (ctx.hit) GetClassNameW(ctx.hit, cls, 64);
    const std::wstring name = cls;
    if (!ctx.hit || name == L"Progman" || name == L"WorkerW") {
        what = L"the desktop";
        return known_folder(FOLDERID_Desktop);
    }
    if (name == L"CabinetWClass") {
        what = L"a folder window";
        return explorer_folder(ctx.hit);
    }
    what = L"a window of class " + name;
    return std::nullopt;
}

std::string narrow(const std::wstring& w) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

// Files the other PC dropped onto this screen at (x, y) in desk mode: into the folder window or onto
// the desktop there; anything else (an app window, a virtual folder) keeps them in Downloads\KVM.
void place_dropped(std::vector<std::filesystem::path> saved, int x, int y) {
    std::thread([saved = std::move(saved), x, y] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::wstring what;
        const auto folder = folder_at(POINT{x, y}, what);
        bool all_moved = false;
        if (folder) {
            const auto placed = move_into(saved, *folder, &all_moved);
            log::info("files: {} item(s) dropped onto {} at ({}, {}): {}", placed.size(), narrow(what), x, y, utf8(*folder));
        }
        if (!folder || !all_moved) {
            if (!folder) log::info("files: dropped onto {} at ({}, {}) - kept in Downloads\\KVM", narrow(what), x, y);
            reveal(keep_in_downloads(saved));
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
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
                // If the drag can't go on here or is cancelled, the files still land in Downloads\KVM.
                if (!drag_source_->start(saved, [saved](DWORD effect) {
                        log::info("files: drag {}", effect != DROPEFFECT_NONE ? "dropped" : "cancelled");
                        if (effect == DROPEFFECT_NONE) reveal(keep_in_downloads(saved));
                    })) {
                    reveal(keep_in_downloads(saved));  // a drag is still running
                }
            },
        // Desk mode: the other PC's user dropped these onto this screen at (x, y).
        .on_dropped_at = [](const std::vector<std::filesystem::path>& saved, int x, int y) { place_dropped(saved, x, y); },
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
    // Desk mode: the drag is over the other PC's screen (the virtual display). Cover it, so the files
    // can't drop onto this PC's own desktop that lies under it, and send them where the user lets go.
    display::Rect vdd;
    bool live = false;
    if (desk_region_ && desk_region_->vdd_rect(vdd, live) && live && vdd.contains({at.x, at.y})) {
        std::vector<RECT> holes;  // this PC's windows on it take their drops themselves
        for (const auto& r : desk_region_->rects()) holes.push_back(RECT{r.left, r.top, r.right, r.bottom});
        log::info("files: a drag is over the other PC's screen - it drops there where you let go");
        catcher_->cover(RECT{vdd.left, vdd.top, vdd.right, vdd.bottom}, std::move(holes),
                        [this, origin = POINT{vdd.left, vdd.top}](std::vector<std::filesystem::path> paths, POINT where) {
                            send_now(std::move(paths), /*drag=*/false,
                                     files::FileTransfer::DropPoint{where.x - origin.x, where.y - origin.y});
                        });
        return;
    }
    log::info("files: a drag reached the other PC's screen, trying to catch it at ({}, {})", at.x, at.y);
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
    send_now(std::move(paths), /*drag=*/false);
}

void FileLink::set_on_received_claim(ReceivedClaim claim) {
    std::lock_guard lock(mutex_);
    on_received_claim_ = std::move(claim);
}

void FileLink::on_catcher_drop(std::vector<std::filesystem::path> paths) {
    send_now(std::move(paths), /*drag=*/GetTickCount64() <= drag_until_.exchange(0));
}

void FileLink::send_now(std::vector<std::filesystem::path> paths, bool drag,
                        std::optional<files::FileTransfer::DropPoint> drop) {
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
        log::info("files: {} {}", drop ? "dropping onto the other PC's screen" : drag ? "dragging across" : "sending", utf8(p));
    }
    channel->send(std::move(paths), drag, drop);
    if (drag && !drop && on_drag_caught) on_drag_caught(drag_y_);  // the cursor follows the files
}

}  // namespace kvm
