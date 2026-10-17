#include "kvm/file_drop.h"

#include <ole2.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <future>
#include <optional>

#include "kvm/log.h"

namespace kvm::files {

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kClassName[] = L"KvmFileDropCatcher";
constexpr UINT kShowMessage = WM_APP + 1;  // wParam = timeout in ms, lParam = MAKELPARAM(x, y)
constexpr UINT kHideMessage = WM_APP + 2;
constexpr UINT_PTR kHideTimer = 1;
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
    DropTarget(HWND hwnd, FileDropCatcher::OnDrop* on_drop) : hwnd_(hwnd), on_drop_(on_drop) {}

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
        *effect = paths.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
        PostMessageW(hwnd_, kHideMessage, 0, 0);
        if (!paths.empty()) {
            log::info("file_drop: {} item(s) dropped", paths.size());
            if (*on_drop_) (*on_drop_)(std::move(paths));
        }
        return S_OK;
    }

    bool dragging() const { return dragging_; }

private:
    ~DropTarget() = default;

    HWND hwnd_;
    FileDropCatcher::OnDrop* on_drop_;
    ULONG refs_ = 1;
    bool has_files_ = false;
    bool dragging_ = false;
};

struct WindowState {
    std::wstring label;
    std::atomic<bool>* visible = nullptr;
    DropTarget* target = nullptr;
    HFONT font = nullptr;
    UINT font_dpi = 0;
};

void hide_window(HWND hwnd, WindowState& state) {
    KillTimer(hwnd, kHideTimer);
    ShowWindow(hwnd, SW_HIDE);
    *state.visible = false;
}

void show_window(HWND hwnd, WindowState& state, POINT pt, UINT timeout_ms) {
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
        case WM_TIMER:
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
        state->target = new DropTarget(hwnd, &on_drop_);
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

}  // namespace kvm::files
