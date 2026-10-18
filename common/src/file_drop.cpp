#include "kvm/file_drop.h"

#include <ole2.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <format>
#include <future>
#include <memory>
#include <optional>

#include "kvm/log.h"
#include "kvm/win32_input.h"

namespace kvm::files {

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kClassName[] = L"KvmFileDropCatcher";
constexpr UINT kShowMessage = WM_APP + 1;  // wParam = timeout in ms, lParam = MAKELPARAM(x, y)
constexpr UINT kHideMessage = WM_APP + 2;
constexpr UINT kCatchMessage = WM_APP + 3;  // lParam = MAKELPARAM(x, y)
constexpr UINT kCoverMessage = WM_APP + 4;  // lParam = CoverRequest* (the catcher thread deletes it)
constexpr UINT_PTR kHideTimer = 1;
constexpr UINT_PTR kReleaseTimer = 2;
constexpr UINT_PTR kGiveUpTimer = 3;
constexpr UINT_PTR kCoverTimer = 4;
constexpr UINT kCoverPollMs = 100;  // cover mode ends once the left button is up and no drag is over us
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

// Accepts drags that carry files (CF_HDROP) and hands their paths over on drop. Called by OLE on the
// catcher's thread.
class DropTarget final : public IDropTarget {
public:
    DropTarget(HWND hwnd, FileDropCatcher::OnDrop* on_drop, std::atomic<bool>* catching, std::atomic<bool>* covering,
               FileDropCatcher::OnDropAt* on_drop_at)
        : hwnd_(hwnd), on_drop_(on_drop), catching_(catching), covering_(covering), on_drop_at_(on_drop_at) {}

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
        // Cover mode stays while the button is down (the drag may come back): kCoverTimer ends it.
        if (!*covering_) SetTimer(hwnd_, kHideTimer, kAfterLeaveMs, nullptr);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        dragging_ = false;
        auto paths = files_in(data);
        *effect = paths.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;  // never a move: the source keeps its files
        const bool covering = *covering_;
        PostMessageW(hwnd_, kHideMessage, 0, 0);
        if (!paths.empty()) {
            log::info("file_drop: {} item(s) dropped{}", paths.size(), covering ? std::format(" at ({}, {})", pt.x, pt.y) : "");
            if (covering && *on_drop_at_) {
                (*on_drop_at_)(std::move(paths), POINT{pt.x, pt.y});
            } else if (*on_drop_) {
                (*on_drop_)(std::move(paths));
            }
        }
        return S_OK;
    }

    bool dragging() const { return dragging_; }

private:
    ~DropTarget() = default;

    HWND hwnd_;
    FileDropCatcher::OnDrop* on_drop_;
    std::atomic<bool>* catching_;
    std::atomic<bool>* covering_;
    FileDropCatcher::OnDropAt* on_drop_at_;
    ULONG refs_ = 1;
    bool has_files_ = false;
    bool dragging_ = false;
};

struct WindowState {
    std::wstring label;
    std::atomic<bool>* visible = nullptr;
    std::atomic<bool> catching = false;  // catch_now() is waiting for a file drag to enter
    std::atomic<bool> covering = false;  // cover(): over the VDD until the drag ends
    FileDropCatcher::OnDropAt on_drop_at;  // cover()'s callback (catcher thread only)
    DropTarget* target = nullptr;
    HFONT font = nullptr;
    UINT font_dpi = 0;
};

// Leaves cover() mode: the next show_at() / catch_now() is a plain box again.
void end_cover(HWND hwnd, WindowState& state) {
    KillTimer(hwnd, kCoverTimer);
    if (state.covering) {
        state.covering = false;
        SetWindowRgn(hwnd, nullptr, FALSE);
    }
}

void hide_window(HWND hwnd, WindowState& state) {
    KillTimer(hwnd, kHideTimer);
    ShowWindow(hwnd, SW_HIDE);
    end_cover(hwnd, state);
    *state.visible = false;
}

// The left button as the user knows it (GetAsyncKeyState sees the physical buttons).
bool left_button_down() {
    return (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
}

void show_window(HWND hwnd, WindowState& state, POINT pt, UINT timeout_ms) {
    end_cover(hwnd, state);
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
void catch_window(HWND hwnd, WindowState& state, POINT pt) {
    end_cover(hwnd, state);
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
    *state.visible = true;
    SetTimer(hwnd, kHideTimer, kCatchTimeoutMs, nullptr);
    // A drag only picks a new drop target when the mouse moves: nudge it so the catcher becomes the
    // target. If it's a file drag, DragEnter arms kReleaseTimer; otherwise kGiveUpTimer leaves the
    // drag (a window or a text selection) alone.
    state.catching = true;
    input::move_cursor_abs(pt.x + 1, pt.y);
    input::move_cursor_abs(pt.x, pt.y);
    SetTimer(hwnd, kGiveUpTimer, kGiveUpMs, nullptr);
}

struct CoverRequest {
    RECT area;
    std::vector<RECT> holes;
    FileDropCatcher::OnDropAt on_drop_at;
};

// Desk mode: an invisible drop target over the whole VDD (the other PC's screen) except this PC's own
// windows on it, which keep taking drops themselves. Nothing is faked: the user's own release drops
// the files onto it wherever over the other screen they let go, and the drop point goes along.
void cover_window(HWND hwnd, WindowState& state, CoverRequest& req) {
    KillTimer(hwnd, kHideTimer);  // an earlier show_at() / catch_now() must not hide the cover mid-drag
    KillTimer(hwnd, kGiveUpTimer);
    KillTimer(hwnd, kReleaseTimer);
    state.catching = false;
    state.on_drop_at = std::move(req.on_drop_at);
    const RECT& a = req.area;
    HRGN region = CreateRectRgn(0, 0, a.right - a.left, a.bottom - a.top);
    for (const RECT& h : req.holes) {
        HRGN hole = CreateRectRgn(h.left - a.left, h.top - a.top, h.right - a.left, h.bottom - a.top);
        CombineRgn(region, region, hole, RGN_DIFF);
        DeleteObject(hole);
    }
    SetLayeredWindowAttributes(hwnd, 0, 1, LWA_ALPHA);  // invisible, but still a drop target
    SetWindowRgn(hwnd, region, FALSE);                   // the window owns the region from now on
    SetWindowPos(hwnd, HWND_TOPMOST, a.left, a.top, a.right - a.left, a.bottom - a.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    state.covering = true;
    *state.visible = true;
    SetTimer(hwnd, kCoverTimer, kCoverPollMs, nullptr);
    // A drag only picks a new drop target when the mouse moves: nudge it, so even a release right
    // now drops onto the cover and not onto this PC's desktop under it.
    POINT cur{};
    if (GetCursorPos(&cur)) {
        input::move_cursor_abs(cur.x + 1, cur.y);
        input::move_cursor_abs(cur.x, cur.y);
    }
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
        case kCoverMessage: {
            std::unique_ptr<CoverRequest> req(reinterpret_cast<CoverRequest*>(lp));
            if (state && req) cover_window(hwnd, *state, *req);
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
            if (wp == kCoverTimer) {
                // The drag ended somewhere else (e.g. back on this PC's own screen): step aside.
                if (state && !left_button_down() && !(state->target && state->target->dragging())) hide_window(hwnd, *state);
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
        state->target = new DropTarget(hwnd, &on_drop_, &state->catching, &state->covering, &state->on_drop_at);
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

void FileDropCatcher::cover(RECT area, std::vector<RECT> holes, OnDropAt on_drop_at) {
    const HWND h = hwnd_;
    if (!h) return;
    auto* req = new CoverRequest{area, std::move(holes), std::move(on_drop_at)};
    if (!PostMessageW(h, kCoverMessage, 0, reinterpret_cast<LPARAM>(req))) delete req;
}

}  // namespace kvm::files
