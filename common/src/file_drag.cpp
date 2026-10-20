// SPDX-License-Identifier: Apache-2.0

#include "kvm/file_drag.h"

#include <ole2.h>
#include <shlobj.h>

#include <chrono>
#include <cstring>
#include <future>
#include <optional>
#include <string>
#include <utility>

#include "kvm/log.h"
#include "kvm/win32_input.h"

namespace kvm::files {

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kClassName[] = L"KvmFileDragSource";
constexpr UINT kStartMessage = WM_APP + 1;
constexpr UINT_PTR kArmTimer = 1;
constexpr UINT kArmTimeoutMs = 700;  // our button press must reach the window by then
constexpr int kSize = 48;            // px; the window only has to be under the cursor

// Moves the cursor to (x, y) and presses the left button there in one input event, so the press
// lands on our window even while the user (or the KVM) keeps moving the cursor: a separate move and
// press let other moves slip in between, and the press then hit whatever was under the cursor.
bool press_left_at(POINT p) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return input::send_mouse_button(input::MouseButton::Left, true);
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = static_cast<LONG>((2LL * (p.x - vx) + 1) * 32768 / vw);  // pixel centre, 0..65535
    in.mi.dy = static_cast<LONG>((2LL * (p.y - vy) + 1) * 32768 / vh);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_LEFTDOWN;
    in.mi.dwExtraInfo = input::kInjectedMarker;  // our own: the KVM hooks let it through untouched
    return SendInput(1, &in, sizeof(in)) == 1;
}

// Drop when the left button goes up, cancel on Esc. Hides the helper window once the drag runs so
// it can't be the drop target itself.
class DropSource final : public IDropSource {
public:
    explicit DropSource(HWND helper) : helper_(helper) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *out = static_cast<IDropSource*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }  // lives on the stack of the drag
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape) return DRAGDROP_S_CANCEL;
        return (keys & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        if (helper_) {
            ShowWindow(helper_, SW_HIDE);
            helper_ = nullptr;
        }
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    HWND helper_;
};

// What the drop target did, as it wrote it into the data object ("Performed DropEffect").
DWORD performed_effect(IDataObject* data) {
    static const CLIPFORMAT format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT));
    FORMATETC f{format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    DWORD effect = DROPEFFECT_NONE;
    if (SUCCEEDED(data->GetData(&f, &medium))) {
        if (const auto* p = static_cast<const DWORD*>(GlobalLock(medium.hGlobal))) {
            if (GlobalSize(medium.hGlobal) >= sizeof(DWORD)) effect = *p;
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }
    return effect;
}

// A 32-bit premultiplied DIB section with the picture, for IDragSourceHelper::InitializeFromBitmap.
HBITMAP make_bitmap(const DragImage& picture) {
    if (picture.width <= 0 || picture.height <= 0 ||
        picture.bgra.size() != static_cast<std::size_t>(picture.width) * picture.height * 4) {
        return nullptr;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = picture.width;
    info.bmiHeader.biHeight = -picture.height;  // top-down, as DragImage keeps the rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits) std::memcpy(bits, picture.bgra.data(), picture.bgra.size());
    return bitmap;
}

// The same data object Explorer builds for these files: CF_HDROP and the shell formats, so any
// folder, the desktop or an app can take the drop. All files must be in one folder.
IDataObject* shell_data_object(const std::vector<fs::path>& files) {
    PIDLIST_ABSOLUTE folder = nullptr;
    if (FAILED(SHParseDisplayName(files.front().parent_path().c_str(), nullptr, &folder, 0, nullptr))) return nullptr;
    std::vector<PIDLIST_ABSOLUTE> items;
    std::vector<PCUITEMID_CHILD> children;
    for (const auto& f : files) {
        PIDLIST_ABSOLUTE item = nullptr;
        if (SUCCEEDED(SHParseDisplayName(f.c_str(), nullptr, &item, 0, nullptr))) {
            items.push_back(item);
            children.push_back(ILFindLastID(item));
        }
    }
    IDataObject* data = nullptr;
    if (!children.empty()) {
        SHCreateDataObject(folder, static_cast<UINT>(children.size()), children.data(), nullptr, IID_PPV_ARGS(&data));
    }
    for (auto* item : items) ILFree(item);
    ILFree(folder);
    return data;
}

}  // namespace

FileDragSource::FileDragSource() {
    std::promise<void> ready;
    auto started = ready.get_future();
    thread_ = std::thread([this, &ready] {
        struct Signal {
            std::promise<void>& p;
            ~Signal() { p.set_value(); }
        };
        std::optional<Signal> signal(std::in_place, ready);
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (FAILED(OleInitialize(nullptr))) {
            log::error("file_drag: OleInitialize failed");
            return;
        }
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);  // fails harmlessly if registered already
        const HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                                          kClassName, L"", WS_POPUP, 0, 0, kSize, kSize, nullptr, nullptr,
                                          GetModuleHandleW(nullptr), this);
        if (!hwnd) {
            log::error("file_drag: CreateWindowEx failed ({})", GetLastError());
            OleUninitialize();
            return;
        }
        SetLayeredWindowAttributes(hwnd, 0, 1, LWA_ALPHA);  // invisible, but still under the mouse
        hwnd_ = hwnd;
        signal.reset();

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        hwnd_ = nullptr;
        OleUninitialize();
    });
    started.wait();
}

FileDragSource::~FileDragSource() {
    if (const HWND h = hwnd_) PostMessageW(h, WM_CLOSE, 0, 0);
    if (thread_.joinable()) thread_.join();
}

bool FileDragSource::start(std::vector<fs::path> files, OnDone on_done, std::optional<DragImage> image) {
    const HWND h = hwnd_;
    if (!h || files.empty()) return false;
    {
        std::lock_guard lock(mutex_);
        if (dragging_) return false;
        pending_ = std::move(files);
        on_done_ = std::move(on_done);
        image_ = std::move(image);
        dragging_ = true;
    }
    PostMessageW(h, kStartMessage, 0, 0);
    return true;
}

void FileDragSource::note_release(POINT at) {
    std::lock_guard lock(mutex_);
    if (!running_) early_release_ = at;
}

void FileDragSource::forget_release() {
    std::lock_guard lock(mutex_);
    if (!running_) early_release_.reset();
}

bool FileDragSource::starting() const {
    std::lock_guard lock(mutex_);
    return dragging_ && !running_;
}

void FileDragSource::on_start(HWND hwnd) {
    POINT cursor{};
    GetCursorPos(&cursor);
    {
        std::lock_guard lock(mutex_);
        if (early_release_) cursor = *early_release_;  // the user let go already: start (and drop) there
    }
    SetWindowPos(hwnd, HWND_TOPMOST, cursor.x - kSize / 2, cursor.y - kSize / 2, kSize, kSize,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    armed_ = true;
    SetTimer(hwnd, kArmTimer, kArmTimeoutMs, nullptr);
    // The press must be ours: only then may this thread capture the mouse for DoDragDrop.
    press_left_at(cursor);
}

void FileDragSource::on_button_down(HWND hwnd) {
    armed_ = false;
    KillTimer(hwnd, kArmTimer);
    std::vector<fs::path> files;
    {
        std::lock_guard lock(mutex_);
        files.swap(pending_);
    }
    IDataObject* data = files.empty() ? nullptr : shell_data_object(files);
    if (!data) {
        log::warn("file_drag: no data object for the files");
        ShowWindow(hwnd, SW_HIDE);
        input::send_mouse_button(input::MouseButton::Left, false);
        finish(DROPEFFECT_NONE);
        return;
    }
    std::optional<POINT> drop_at;
    std::optional<DragImage> picture;
    {
        std::lock_guard lock(mutex_);
        running_ = true;
        drop_at = std::exchange(early_release_, std::nullopt);
        picture = std::exchange(image_, std::nullopt);
    }
    // The picture under the cursor and the "Copy to ..." text drop targets show beneath it: the one
    // the other PC's Explorer drew for these files, so the drag looks exactly as it did there (as
    // when files go from one monitor to another); else the shell's own picture of them.
    IDragSourceHelper2* helper = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper)))) {
        helper->SetFlags(DSH_ALLOWDROPDESCRIPTIONTEXT);
        bool shown = false;
        if (picture) {
            if (HBITMAP bitmap = make_bitmap(*picture)) {
                SHDRAGIMAGE image{{picture->width, picture->height}, {picture->cursor_x, picture->cursor_y}, bitmap,
                                  CLR_NONE};
                shown = SUCCEEDED(helper->InitializeFromBitmap(&image, data));  // the helper owns it now
                if (!shown) DeleteObject(bitmap);
            }
        }
        if (!shown) {
            POINT at{};
            GetCursorPos(&at);
            if (const HRESULT ih = helper->InitializeFromWindow(nullptr, &at, data); FAILED(ih)) {
                log::warn("file_drag: no drag image ({:#x})", static_cast<unsigned>(ih));
            }
        }
        helper->Release();
    }
    if (drop_at) {
        // The user let go before the drag got here: drop where they did, once the drag runs (a move
        // lets it find the target there, then the button goes up).
        log::info("file_drag: the button went up before the files arrived - dropping them at ({}, {})", drop_at->x,
                  drop_at->y);
        std::thread([at = *drop_at] {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            input::move_cursor_abs(at.x + 1, at.y);
            input::move_cursor_abs(at.x, at.y);
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            input::send_mouse_button(input::MouseButton::Left, false);
        }).detach();
    }
    DropSource source(hwnd);
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT hr = SHDoDragDrop(hwnd, data, &source, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);
    if (hr == DRAGDROP_S_DROP && effect == DROPEFFECT_NONE) {
        // An optimized move: Explorer moved the files itself (same drive) and says so in the data
        // object, while the effect stays "none" so the source doesn't delete them a second time.
        effect = performed_effect(data);
    }
    data->Release();
    ShowWindow(hwnd, SW_HIDE);
    if (hr != DRAGDROP_S_DROP) effect = DROPEFFECT_NONE;
    // Where it ended, so a drop that didn't take can be told apart (no target there, or Esc).
    POINT end{};
    GetCursorPos(&end);
    wchar_t cls[64]{};
    if (const HWND under = WindowFromPoint(end)) GetClassNameW(GetAncestor(under, GA_ROOT), cls, 64);
    const int n = WideCharToMultiByte(CP_UTF8, 0, cls, -1, nullptr, 0, nullptr, nullptr);
    std::string over(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, cls, -1, over.data(), n, nullptr, nullptr);
    log::info("file_drag: {} item(s) {} at ({}, {}) over a {} window ({:#x})", files.size(),
              effect != DROPEFFECT_NONE ? "dropped" : "not dropped", end.x, end.y, over.empty() ? "?" : over,
              static_cast<unsigned>(hr));
    finish(effect);
}

void FileDragSource::finish(DWORD effect) {
    OnDone done;
    {
        std::lock_guard lock(mutex_);
        done = std::move(on_done_);
        dragging_ = false;
        running_ = false;
        early_release_.reset();
    }
    if (done) done(effect);
}

LRESULT CALLBACK FileDragSource::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<FileDragSource*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
            break;
        case kStartMessage:
            if (self) self->on_start(hwnd);
            return 0;
        case WM_LBUTTONDOWN:
            if (self && self->armed_) self->on_button_down(hwnd);
            return 0;
        case WM_TIMER:
            if (self && wp == kArmTimer && self->armed_) {
                // Our press never reached the window (something else was under the cursor).
                self->armed_ = false;
                KillTimer(hwnd, kArmTimer);
                ShowWindow(hwnd, SW_HIDE);
                input::send_mouse_button(input::MouseButton::Left, false);
                log::warn("file_drag: the drag could not start here");
                self->finish(DROPEFFECT_NONE);
            }
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace kvm::files
