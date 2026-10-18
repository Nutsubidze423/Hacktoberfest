#include "kvm/file_drop.h"

#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <future>
#include <memory>
#include <optional>
#include <utility>

#include "kvm/log.h"
#include "kvm/win32_input.h"

namespace kvm::files {

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kClassName[] = L"KvmFileDropCatcher";
constexpr UINT kShowMessage = WM_APP + 1;  // wParam = timeout in ms, lParam = MAKELPARAM(x, y)
constexpr UINT kHideMessage = WM_APP + 2;
constexpr UINT kCatchMessage = WM_APP + 3;  // lParam = MAKELPARAM(x, y)
constexpr UINT kCatchOverMessage = WM_APP + 4;  // lParam = CatchOverRequest* (the catcher thread deletes it)
constexpr UINT_PTR kHideTimer = 1;
constexpr UINT_PTR kReleaseTimer = 2;
constexpr UINT_PTR kGiveUpTimer = 3;
constexpr int kCatchSizePx = 96;  // roomy: the cursor may slide along the edge before the release
constexpr UINT kCatchTimeoutMs = 1500;
constexpr UINT kReleaseAfterMs = 30;  // after the file drag entered the catcher
constexpr UINT kGiveUpMs = 250;       // no file drag entered by then: leave the user's drag alone
constexpr int kWidthDp = 240;
constexpr int kHeightDp = 64;
constexpr UINT kAfterLeaveMs = 700;  // a drag that left may come back

FORMATETC hdrop_format() {
    return {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
}

std::vector<fs::path> files_in(IDataObject* data) {
    std::vector<fs::path> out;
    FORMATETC format = hdrop_format();
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&format, &medium))) return out;
    const auto drop = static_cast<HDROP>(medium.hGlobal);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        const UINT length = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(length, L'\0');
        DragQueryFileW(drop, i, path.data(), length + 1);
        out.emplace_back(std::move(path));
    }
    ReleaseStgMedium(&medium);
    return out;
}

// The drag image the shell keeps in the data object ("DragImageBits", written by IDragSourceHelper,
// i.e. by Explorer). The same on 32- and 64-bit Windows: a 24-byte header - SIZE size, POINT cursor
// offset, a 32-bit bitmap handle, COLORREF colour key - then the premultiplied 32-bit pixels,
// bottom-up. nullopt if the drag has none.
std::optional<DragImage> drag_image_in(IDataObject* data) {
    static const CLIPFORMAT format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"DragImageBits"));
    constexpr std::size_t kHeaderBytes = 24;
    FORMATETC f{format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (!format || FAILED(data->GetData(&f, &medium))) return std::nullopt;
    std::optional<DragImage> out;
    const SIZE_T size = GlobalSize(medium.hGlobal);
    if (const auto* p = static_cast<const std::uint8_t*>(GlobalLock(medium.hGlobal))) {
        std::int32_t head[4]{};  // width, height, cursor x, cursor y
        if (size >= kHeaderBytes) std::memcpy(head, p, sizeof(head));
        const int w = head[0], h = head[1];
        const std::size_t pixels = static_cast<std::size_t>(w > 0 ? w : 0) * static_cast<std::size_t>(h > 0 ? h : 0) * 4;
        if (w > 0 && h > 0 && w <= 1024 && h <= 1024 && size >= kHeaderBytes + pixels) {
            DragImage image{.width = w, .height = h, .cursor_x = head[2], .cursor_y = head[3]};
            image.bgra.resize(pixels);
            const std::size_t row = static_cast<std::size_t>(w) * 4;
            const std::uint8_t* bits = p + kHeaderBytes;
            for (int y = 0; y < h; ++y) std::memcpy(image.bgra.data() + y * row, bits + (h - 1 - y) * row, row);
            out = std::move(image);
        } else if (w != 0 || h != 0) {  // 0x0: the drag has no picture
            log::warn("file_drop: drag image not understood ({} bytes, {}x{})", size, w, h);
        }
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return out;
}

// Accepts drags that carry files (CF_HDROP) and hands their paths over on drop. Called by OLE on the
// catcher's thread.
class DropTarget final : public IDropTarget {
public:
    DropTarget(HWND hwnd, FileDropCatcher::OnDrop* on_drop, std::atomic<bool>* catching, std::mutex* image_mutex,
               std::optional<DragImage>* image)
        : hwnd_(hwnd), on_drop_(on_drop), catching_(catching), image_mutex_(image_mutex), image_(image) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --refs_;
        if (refs == 0) delete this;
        return refs;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        FORMATETC format = hdrop_format();
        has_files_ = data->QueryGetData(&format) == S_OK;
        dragging_ = true;
        *effect = has_files_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        // catch_now(): only a drag that carries files gets its button released here.
        if (has_files_ && *catching_) SetTimer(hwnd_, kReleaseTimer, kReleaseAfterMs, nullptr);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect = has_files_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        dragging_ = false;
        SetTimer(hwnd_, kHideTimer, kAfterLeaveMs, nullptr);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        dragging_ = false;
        auto paths = files_in(data);
        *effect = paths.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;  // never a move: the source keeps its files
        PostMessageW(hwnd_, kHideMessage, 0, 0);
        if (!paths.empty()) {
            auto image = drag_image_in(data);
            log::info("file_drop: {} item(s) dropped{}", paths.size(),
                      image ? std::format(", drag image {}x{}", image->width, image->height) : "");
            {
                std::lock_guard lock(*image_mutex_);
                *image_ = std::move(image);
            }
            if (*on_drop_) (*on_drop_)(std::move(paths));
        }
        return S_OK;
    }

    bool dragging() const { return dragging_; }

private:
    ~DropTarget() = default;

    HWND hwnd_;
    FileDropCatcher::OnDrop* on_drop_;
    std::atomic<bool>* catching_;
    std::mutex* image_mutex_;
    std::optional<DragImage>* image_;
    ULONG refs_ = 1;
    bool has_files_ = false;
    bool dragging_ = false;
};

struct WindowState {
    std::wstring label;
    std::atomic<bool>* visible = nullptr;
    std::atomic<bool> catching = false;  // catch_now() / catch_over() is waiting for a file drag to enter
    bool shaped = false;                 // catch_over() gave the window a region (holes)
    DropTarget* target = nullptr;
    HFONT font = nullptr;
    UINT font_dpi = 0;
};

// Back to a plain rectangle after catch_over().
void unshape(HWND hwnd, WindowState& state) {
    if (!state.shaped) return;
    state.shaped = false;
    SetWindowRgn(hwnd, nullptr, FALSE);
}

void hide_window(HWND hwnd, WindowState& state) {
    KillTimer(hwnd, kHideTimer);
    ShowWindow(hwnd, SW_HIDE);
    unshape(hwnd, state);
    *state.visible = false;
}

void show_window(HWND hwnd, WindowState& state, POINT pt, UINT timeout_ms) {
    unshape(hwnd, state);
    const HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    UINT dpi_x = 96, dpi_y = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
    if (state.font_dpi != dpi_x) {
        if (state.font) DeleteObject(state.font);
        state.font = CreateFontW(-MulDiv(16, static_cast<int>(dpi_x), 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, L"Segoe UI");
        state.font_dpi = dpi_x;
    }
    SetLayeredWindowAttributes(hwnd, 0, 235, LWA_ALPHA);  // catch_now() may have made it invisible
    const int w = MulDiv(kWidthDp, static_cast<int>(dpi_x), 96);
    const int h = MulDiv(kHeightDp, static_cast<int>(dpi_x), 96);
    // Centered on the point but kept on its monitor, so a point on the very edge is still covered.
    const RECT& m = info.rcMonitor;
    const int x = std::clamp(static_cast<int>(pt.x) - w / 2, static_cast<int>(m.left), static_cast<int>(m.right) - w);
    const int y = std::clamp(static_cast<int>(pt.y) - h / 2, static_cast<int>(m.top), static_cast<int>(m.bottom) - h);
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd, nullptr, TRUE);
    *state.visible = true;
    SetTimer(hwnd, kHideTimer, timeout_ms, nullptr);
}

// An invisible catcher right under the cursor, then the left button goes up there: the running drag
// drops onto it. alpha 1 keeps it invisible but still hit-testable for the drop.
// The window is in place (invisible: alpha 1 keeps it hit-testable for the drop): wait for the file
// drag. A drag only picks a new drop target when the mouse moves, so nudge it at `at` (the cursor) to
// make the catcher the target. If it's a file drag, DragEnter arms kReleaseTimer; otherwise
// kGiveUpTimer leaves the drag (a window or a text selection) alone.
void start_catching(HWND hwnd, WindowState& state, POINT at) {
    *state.visible = true;
    SetTimer(hwnd, kHideTimer, kCatchTimeoutMs, nullptr);
    state.catching = true;
    input::move_cursor_abs(at.x + 1, at.y);
    input::move_cursor_abs(at.x, at.y);
    SetTimer(hwnd, kGiveUpTimer, kGiveUpMs, nullptr);
}

// An invisible catcher right under the cursor, then the left button goes up there: the running drag
// drops onto it.
void catch_window(HWND hwnd, WindowState& state, POINT pt) {
    unshape(hwnd, state);
    const HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    const RECT& m = info.rcMonitor;
    const int x = std::clamp(static_cast<int>(pt.x) - kCatchSizePx / 2, static_cast<int>(m.left),
                             static_cast<int>(m.right) - kCatchSizePx);
    const int y = std::clamp(static_cast<int>(pt.y) - kCatchSizePx / 2, static_cast<int>(m.top),
                             static_cast<int>(m.bottom) - kCatchSizePx);
    SetLayeredWindowAttributes(hwnd, 0, 1, LWA_ALPHA);
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, kCatchSizePx, kCatchSizePx, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    start_catching(hwnd, state, pt);
}

struct CatchOverRequest {
    RECT area;
    std::vector<RECT> holes;
};

// Desk mode's catch_window(): the catcher covers the whole VDD (the other PC's screen) except this
// PC's own windows on it, which keep taking drops themselves. However the cursor moves until the
// release, it is still over the catcher, so the files can't fall onto this PC's desktop under the VDD.
void catch_over_window(HWND hwnd, WindowState& state, const CatchOverRequest& req) {
    const RECT& a = req.area;
    HRGN region = CreateRectRgn(0, 0, a.right - a.left, a.bottom - a.top);
    for (const RECT& h : req.holes) {
        HRGN hole = CreateRectRgn(h.left - a.left, h.top - a.top, h.right - a.left, h.bottom - a.top);
        CombineRgn(region, region, hole, RGN_DIFF);
        DeleteObject(hole);
    }
    SetLayeredWindowAttributes(hwnd, 0, 1, LWA_ALPHA);
    SetWindowRgn(hwnd, region, FALSE);  // the window owns the region from now on
    state.shaped = true;
    SetWindowPos(hwnd, HWND_TOPMOST, a.left, a.top, a.right - a.left, a.bottom - a.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    POINT cur{};
    GetCursorPos(&cur);
    start_catching(hwnd, state, cur);
}

void paint(HWND hwnd, WindowState& state) {
    PAINTSTRUCT ps;
    const HDC dc = BeginPaint(hwnd, &ps);
    RECT r;
    GetClientRect(hwnd, &r);
    const HBRUSH background = CreateSolidBrush(RGB(0x1E, 0x24, 0x30));
    FillRect(dc, &r, background);
    DeleteObject(background);
    const HBRUSH accent = CreateSolidBrush(RGB(0x4C, 0x8D, 0xFF));
    FrameRect(dc, &r, accent);
    DeleteObject(accent);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xF2, 0xF4, 0xF8));
    const HGDIOBJ old_font = state.font ? SelectObject(dc, state.font) : nullptr;
    DrawTextW(dc, state.label.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (old_font) SelectObject(dc, old_font);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
            break;
        case kShowMessage:
            if (state) show_window(hwnd, *state, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, static_cast<UINT>(wp));
            return 0;
        case kHideMessage:
            if (state) hide_window(hwnd, *state);
            return 0;
        case kCatchMessage:
            if (state) catch_window(hwnd, *state, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case kCatchOverMessage: {
            std::unique_ptr<CatchOverRequest> req(reinterpret_cast<CatchOverRequest*>(lp));
            if (state && req) catch_over_window(hwnd, *state, *req);
            return 0;
        }
        case WM_TIMER:
            if (wp == kReleaseTimer) {
                KillTimer(hwnd, kReleaseTimer);
                KillTimer(hwnd, kGiveUpTimer);
                if (state) state->catching = false;
                input::send_mouse_button(input::MouseButton::Left, false);  // the file drag drops onto us
                return 0;
            }
            if (wp == kGiveUpTimer) {
                KillTimer(hwnd, kGiveUpTimer);
                if (state && state->catching) {
                    state->catching = false;
                    hide_window(hwnd, *state);
                    log::info("file_drop: no file drag arrived here, left the drag alone");
                }
                return 0;
            }
            if (state && wp == kHideTimer) {
                if (state->target && state->target->dragging()) {
                    SetTimer(hwnd, kHideTimer, 500, nullptr);  // a drag is over us: stay
                } else {
                    hide_window(hwnd, *state);
                }
            }
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_PAINT:
            if (state) {
                paint(hwnd, *state);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            RevokeDragDrop(hwnd);
            if (state) {
                if (state->target) state->target->Release();
                if (state->font) DeleteObject(state->font);
                delete state;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

FileDropCatcher::FileDropCatcher(OnDrop on_drop, std::wstring label)
    : on_drop_(std::move(on_drop)), label_(std::move(label)) {
    std::promise<void> ready;
    auto started = ready.get_future();
    thread_ = std::thread([this, &ready] {
        // Signals the constructor as soon as hwnd_ is set (or creation failed), then runs the loop.
        struct Signal {
            std::promise<void>& p;
            ~Signal() { p.set_value(); }
        };
        std::optional<Signal> signal(std::in_place, ready);
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (FAILED(OleInitialize(nullptr))) {
            log::error("file_drop: OleInitialize failed");
            return;
        }
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);  // fails harmlessly if another catcher registered it already

        auto* state = new WindowState{label_, &visible_};
        const HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                                          kClassName, label_.c_str(), WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                          GetModuleHandleW(nullptr), state);
        if (!hwnd) {
            log::error("file_drop: CreateWindowEx failed ({})", GetLastError());
            delete state;
            OleUninitialize();
            return;
        }
        SetLayeredWindowAttributes(hwnd, 0, 235, LWA_ALPHA);
        state->target = new DropTarget(hwnd, &on_drop_, &state->catching, &image_mutex_, &drag_image_);
        if (FAILED(RegisterDragDrop(hwnd, state->target))) log::error("file_drop: RegisterDragDrop failed");
        hwnd_ = hwnd;
        signal.reset();  // the constructor may return now

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

FileDropCatcher::~FileDropCatcher() {
    if (const HWND h = hwnd_) PostMessageW(h, WM_CLOSE, 0, 0);
    if (thread_.joinable()) thread_.join();
}

void FileDropCatcher::show_at(POINT screen_point, std::chrono::milliseconds timeout) {
    if (const HWND h = hwnd_) {
        PostMessageW(h, kShowMessage, static_cast<WPARAM>(timeout.count()), MAKELPARAM(screen_point.x, screen_point.y));
    }
}

void FileDropCatcher::hide() {
    if (const HWND h = hwnd_) PostMessageW(h, kHideMessage, 0, 0);
}

void FileDropCatcher::catch_now(POINT screen_point) {
    if (const HWND h = hwnd_) PostMessageW(h, kCatchMessage, 0, MAKELPARAM(screen_point.x, screen_point.y));
}

std::optional<DragImage> FileDropCatcher::take_drag_image() {
    std::lock_guard lock(image_mutex_);
    return std::exchange(drag_image_, std::nullopt);
}

void FileDropCatcher::catch_over(RECT area, std::vector<RECT> holes) {
    const HWND h = hwnd_;
    if (!h) return;
    auto req = std::make_unique<CatchOverRequest>(CatchOverRequest{area, std::move(holes)});
    if (PostMessageW(h, kCatchOverMessage, 0, reinterpret_cast<LPARAM>(req.get()))) req.release();  // the catcher thread owns it now
}

}  // namespace kvm::files
