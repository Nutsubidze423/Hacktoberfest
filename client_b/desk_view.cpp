#include "desk_view.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <span>
#include <string>
#include <utility>

#include "jpeg_decoder.h"
#include "kvm/log.h"
#include "kvm/win32_input.h"

namespace kvm {

namespace {

constexpr wchar_t kClassName[] = L"KvmDeskView";
constexpr UINT kMsgUpdate = WM_APP + 1;
constexpr UINT kMsgSave = WM_APP + 2;  // wparam = const std::wstring* path; returns 1 on success

// The WinEvent callback runs on the UI thread that registered the hook, so the DeskView on that
// thread can be found without a user-data parameter (there is one DeskView per UI thread).
thread_local DeskView* t_self = nullptr;

bool injected_by_us() {
    return input::is_own_injection(static_cast<std::uintptr_t>(GetMessageExtraInfo()));
}

RECT primary_rect() {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcMonitor;
}

void unite(RECT& into, const RECT& r) {
    if (IsRectEmpty(&r)) return;
    if (IsRectEmpty(&into)) {
        into = r;
    } else {
        UnionRect(&into, &into, &r);
    }
}

LPCWSTR cursor_id(std::string_view shape) {
    if (shape == "ibeam") return IDC_IBEAM;
    if (shape == "hand") return IDC_HAND;
    if (shape == "wait") return IDC_WAIT;
    if (shape == "appstarting") return IDC_APPSTARTING;
    if (shape == "cross") return IDC_CROSS;
    if (shape == "sizewe") return IDC_SIZEWE;
    if (shape == "sizens") return IDC_SIZENS;
    if (shape == "sizenwse") return IDC_SIZENWSE;
    if (shape == "sizenesw") return IDC_SIZENESW;
    if (shape == "sizeall") return IDC_SIZEALL;
    if (shape == "no") return IDC_NO;
    return IDC_ARROW;
}

// Applies the run-length alpha (see proto::FrameHeader) to a decoded picture. False if the runs
// don't fit the picture (then it stays opaque).
bool apply_alpha(std::span<const std::uint8_t> runs, int width, int height, std::vector<std::uint8_t>& px,
                 std::vector<std::uint8_t>& opaque_rows) {
    size_t i = 0;
    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = px.data() + static_cast<size_t>(y) * width * 4;
        int x = 0;
        bool opaque = true;
        while (x < width) {
            if (i + 3 > runs.size()) return false;
            const std::uint8_t a = runs[i];
            const int n = runs[i + 1] | (runs[i + 2] << 8);
            i += 3;
            if (n <= 0 || x + n > width) return false;
            if (a != 255) {
                opaque = false;
                for (int k = x; k < x + n; ++k) {
                    std::uint8_t* p = row + static_cast<size_t>(k) * 4;
                    // Premultiplied: no colour above its alpha (JPEG noise could make one glow).
                    p[0] = std::min(p[0], a);
                    p[1] = std::min(p[1], a);
                    p[2] = std::min(p[2], a);
                    p[3] = a;
                }
            }
            x += n;
        }
        opaque_rows[static_cast<size_t>(y)] = opaque ? 1 : 0;
    }
    return true;
}

}  // namespace

DeskView::DeskView() {
    cursor_ = LoadCursorW(nullptr, IDC_ARROW);
    input_.set_sink([this](const proto::Message& m) {
        Sink sink;
        {
            std::lock_guard lock(sink_mutex_);
            sink = sink_;
        }
        return sink && sink(m);
    });
    std::promise<void> ready;
    auto started = ready.get_future();
    ui_thread_ = std::thread([this, &ready] { ui_main(&ready); });
    started.wait();
    decode_thread_ = std::jthread([this](std::stop_token stop) { decode_main(stop); });
}

DeskView::~DeskView() {
    decode_thread_.request_stop();
    incoming_cv_.notify_all();
    if (decode_thread_.joinable()) decode_thread_.join();
    if (const DWORD id = ui_thread_id_) PostThreadMessageW(id, WM_QUIT, 0, 0);
    if (ui_thread_.joinable()) ui_thread_.join();
}

void DeskView::present(net::Frame&& frame) {
    {
        std::lock_guard lock(incoming_mutex_);
        incoming_[frame.header.window] = std::move(frame);
    }
    incoming_cv_.notify_one();
}

void DeskView::set_layout(proto::DeskLayout layout) {
    {
        std::lock_guard lock(pending_mutex_);
        pending_layout_ = std::move(layout);
    }
    if (const HWND h = hwnd_) PostMessageW(h, kMsgUpdate, 0, 0);
}

void DeskView::clear() {
    {
        std::lock_guard lock(incoming_mutex_);
        incoming_.clear();
    }
    {
        std::lock_guard lock(pending_mutex_);
        pending_layout_.reset();
        pending_pictures_.clear();
        pending_clear_ = true;
    }
    if (const HWND h = hwnd_) PostMessageW(h, kMsgUpdate, 0, 0);
}

bool DeskView::save_surface(const std::wstring& path) {
    const HWND h = hwnd_;
    return h && SendMessageW(h, kMsgSave, reinterpret_cast<WPARAM>(&path), 0) == 1;
}

void DeskView::set_input_sink(Sink sink) {
    std::lock_guard lock(sink_mutex_);
    sink_ = std::move(sink);
}

void DeskView::set_cursor_shape(std::string_view shape) {
    cursor_ = LoadCursorW(nullptr, cursor_id(shape));
}

void DeskView::decode_main(std::stop_token stop) {
    JpegDecoder decoder;
    BgraImage image;
    while (!stop.stop_requested()) {
        std::map<std::uint64_t, net::Frame> frames;
        {
            std::unique_lock lock(incoming_mutex_);
            incoming_cv_.wait(lock, [&] { return stop.stop_requested() || !incoming_.empty(); });
            frames.swap(incoming_);
        }
        for (auto& [id, frame] : frames) {
            const auto& h = frame.header;
            if (h.codec != "jpeg" || h.alpha_bytes > frame.payload.size()) continue;
            const size_t jpeg_size = frame.payload.size() - h.alpha_bytes;
            if (!decoder.decode(std::span(frame.payload.data(), jpeg_size), image)) {
                log::warn("desk: picture {} dropped: {}", h.seq, decoder.last_error());
                continue;
            }
            Picture pic{.width = image.width, .height = image.height, .x = h.x, .y = h.y};
            pic.pixels = std::move(image.pixels);
            pic.opaque_rows.assign(static_cast<size_t>(pic.height), 1);
            if (h.alpha_bytes > 0 &&
                !apply_alpha(std::span(frame.payload.data() + jpeg_size, h.alpha_bytes), pic.width, pic.height,
                             pic.pixels, pic.opaque_rows)) {
                log::warn("desk: picture {}: bad transparency, shown opaque", h.seq);
            }
            image = {};  // the pixels moved out
            {
                std::lock_guard lock(pending_mutex_);
                pending_pictures_[id] = std::move(pic);
            }
        }
        if (const HWND h = hwnd_) PostMessageW(h, kMsgUpdate, 0, 0);
    }
}

void DeskView::ui_main(std::promise<void>* ready) {
    ui_thread_id_ = GetCurrentThreadId();
    t_self = this;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &DeskView::wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);  // fails harmlessly if a second DeskView registered it already

    screen_ = primary_rect();
    // Not topmost: the overlay rides in the normal z-order so this PC's own windows can come in front
    // of the owner's windows when the user raises them (apply_z keeps it above the desktop either way).
    const HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"Sarke desk",
                                      WS_POPUP, screen_.left, screen_.top, screen_.right - screen_.left,
                                      screen_.bottom - screen_.top, nullptr, nullptr, instance, this);
    if (!hwnd) log::error("desk: the overlay window could not be created ({})", GetLastError());
    hwnd_ = hwnd;
    if (hwnd && !make_surface()) log::error("desk: no drawing surface");
    // Learn when the user raises one of this PC's own windows, so the overlay drops below it.
    foreground_hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &on_foreground_event,
                                       0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    ready->set_value();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (foreground_hook_) UnhookWinEvent(foreground_hook_);
    foreground_hook_ = nullptr;
    hwnd_ = nullptr;
    if (hwnd) DestroyWindow(hwnd);
    free_surface();
    t_self = nullptr;
}

void DeskView::on_foreground_event(HWINEVENTHOOK, DWORD, HWND hwnd, LONG id_object, LONG id_child, DWORD, DWORD) {
    if (t_self && hwnd && id_object == OBJID_WINDOW && id_child == CHILDID_SELF) t_self->on_local_foreground(hwnd);
}

void DeskView::on_local_foreground(HWND fg) {
    if (fg == hwnd_ || GetAncestor(fg, GA_ROOT) != fg) return;  // our overlay or a child control
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return;  // one of our own windows (WINEVENT_SKIPOWNPROCESS should already skip it)
    local_foreground_ = fg;
    front_ = Front::Local;  // the user raised a window of this PC: let it cover the owner's windows
    if (visible_) apply_z();
}

void DeskView::apply_z() {
    HWND after = HWND_TOP;  // front_ == Owner: above this PC's normal windows (but not topmost)
    if (front_ == Front::Local && local_foreground_ && IsWindow(local_foreground_) && local_foreground_ != hwnd_) {
        after = local_foreground_;  // just below the window the user raised, still above the desktop
    }
    UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (!visible_) flags |= SWP_SHOWWINDOW;
    SetWindowPos(hwnd_, after, 0, 0, 0, 0, flags);
    visible_ = true;
}

bool DeskView::make_surface() {
    free_surface();
    screen_ = primary_rect();
    const int w = screen_.right - screen_.left;
    const int h = screen_.bottom - screen_.top;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    mem_dc_ = CreateCompatibleDC(nullptr);
    bitmap_ = CreateDIBSection(mem_dc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!mem_dc_ || !bitmap_ || !bits) {
        free_surface();
        return false;
    }
    old_bitmap_ = SelectObject(mem_dc_, bitmap_);
    bits_ = static_cast<std::uint8_t*>(bits);
    std::memset(bits_, 0, static_cast<size_t>(w) * h * 4);  // fully transparent
    placed_.clear();
    first_update_ = true;
    return true;
}

void DeskView::free_surface() {
    if (mem_dc_ && old_bitmap_) SelectObject(mem_dc_, old_bitmap_);
    if (bitmap_) DeleteObject(bitmap_);
    if (mem_dc_) DeleteDC(mem_dc_);
    mem_dc_ = nullptr;
    bitmap_ = nullptr;
    old_bitmap_ = nullptr;
    bits_ = nullptr;
}

void DeskView::update() {
    std::optional<proto::DeskLayout> layout;
    std::map<std::uint64_t, Picture> pictures;
    bool clear = false;
    {
        std::lock_guard lock(pending_mutex_);
        layout = std::exchange(pending_layout_, std::nullopt);
        pictures.swap(pending_pictures_);
        clear = std::exchange(pending_clear_, false);
    }
    if (clear) {
        hide();
        layout_ = {};
        pictures_.clear();
    }
    if (!layout && pictures.empty()) return;
    const HWND hwnd = hwnd_;
    if (!hwnd || !bits_) return;

    std::set<std::uint64_t> new_pictures;
    for (auto& [id, pic] : pictures) {
        new_pictures.insert(id);
        pictures_[id] = std::move(pic);
    }
    bool raise_owner = false;
    if (layout) {
        layout_ = std::move(*layout);
        // The owner's active window is in front over there: bring our overlay above our own windows.
        if (layout_.active && front_ != Front::Owner) {
            front_ = Front::Owner;
            raise_owner = true;
        }
        // Forget windows that are gone.
        std::set<std::uint64_t> alive;
        for (const auto& w : layout_.windows) alive.insert(w.id);
        std::erase_if(pictures_, [&](const auto& kv) { return !alive.contains(kv.first); });
    }

    // Where each window goes now, bottom first, clipped to the owner's virtual display and our screen.
    const int sw = screen_.right - screen_.left;
    const int sh = screen_.bottom - screen_.top;
    const RECT clip{0, 0, std::min(layout_.desk_w, sw), std::min(layout_.desk_h, sh)};
    std::vector<Placed> placed;
    for (auto it = layout_.windows.rbegin(); it != layout_.windows.rend(); ++it) {
        const auto pic = pictures_.find(it->id);
        if (pic == pictures_.end()) continue;  // its first picture is still on the way
        const RECT full{it->x + pic->second.x, it->y + pic->second.y, it->x + pic->second.x + pic->second.width,
                        it->y + pic->second.y + pic->second.height};
        RECT r{};
        if (!IntersectRect(&r, &full, &clip)) continue;
        placed.push_back({.id = it->id, .rect = r, .src_x = r.left - full.left, .src_y = r.top - full.top});
    }

    // Redraw only what changed: windows that moved, appeared, vanished or got a new picture.
    RECT dirty{};
    const auto find = [](const std::vector<Placed>& v, std::uint64_t id) -> const Placed* {
        for (const auto& p : v) {
            if (p.id == id) return &p;
        }
        return nullptr;
    };
    for (const auto& p : placed) {
        const Placed* before = find(placed_, p.id);
        if (!before || !EqualRect(&before->rect, &p.rect) || before->src_x != p.src_x || before->src_y != p.src_y ||
            new_pictures.contains(p.id)) {
            unite(dirty, p.rect);
            if (before) unite(dirty, before->rect);
        }
    }
    for (const auto& p : placed_) {
        if (!find(placed, p.id)) unite(dirty, p.rect);
    }
    // The order changed (a window came to the front): redraw where they overlap.
    bool set_changed = placed.size() != placed_.size();
    if (!set_changed) {
        for (size_t i = 0; i < placed.size(); ++i) {
            if (placed[i].id != placed_[i].id) {
                set_changed = true;
                unite(dirty, placed[i].rect);
                unite(dirty, placed_[i].rect);
            }
        }
    }
    placed_ = std::move(placed);
    if (placed_.empty()) {
        hide();
        return;
    }
    if (IsRectEmpty(&dirty)) return;
    compose(placed_, dirty);

    POINT dst{screen_.left, screen_.top};
    SIZE size{sw, sh};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UPDATELAYEREDWINDOWINFO info{sizeof(info)};
    info.pptDst = &dst;
    info.psize = &size;
    info.hdcSrc = mem_dc_;
    info.pptSrc = &src;
    info.pblend = &blend;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = first_update_ ? nullptr : &dirty;
    if (!UpdateLayeredWindowIndirect(hwnd, &info)) {
        log::warn("desk: overlay update failed ({})", GetLastError());
        first_update_ = true;
        return;
    }
    first_update_ = false;
    ++shown_;
    if (!visible_ || set_changed || raise_owner) apply_z();
}

void DeskView::compose(const std::vector<Placed>& placed, const RECT& dirty) {
    const size_t pitch = static_cast<size_t>(screen_.right - screen_.left) * 4;
    for (int y = dirty.top; y < dirty.bottom; ++y) {
        std::memset(bits_ + y * pitch + static_cast<size_t>(dirty.left) * 4, 0, static_cast<size_t>(dirty.right - dirty.left) * 4);
    }
    for (const auto& p : placed) {  // bottom first: each one over what is below it
        RECT r{};
        if (!IntersectRect(&r, &p.rect, &dirty)) continue;
        const Picture& pic = pictures_.at(p.id);
        const int sx = p.src_x + (r.left - p.rect.left);
        const int sy = p.src_y + (r.top - p.rect.top);
        const int w = r.right - r.left;
        for (int y = 0; y < r.bottom - r.top; ++y) {
            std::uint8_t* dst = bits_ + (r.top + y) * pitch + static_cast<size_t>(r.left) * 4;
            const std::uint8_t* src = pic.pixels.data() + (static_cast<size_t>(sy + y) * pic.width + sx) * 4;
            if (pic.opaque_rows[static_cast<size_t>(sy + y)]) {
                std::memcpy(dst, src, static_cast<size_t>(w) * 4);
                continue;
            }
            for (int x = 0; x < w; ++x, dst += 4, src += 4) {  // premultiplied "over"
                const unsigned a = src[3];
                if (a == 255) {
                    std::memcpy(dst, src, 4);
                } else if (a != 0) {
                    const unsigned keep = 255 - a;
                    for (int c = 0; c < 4; ++c) dst[c] = static_cast<std::uint8_t>(src[c] + (dst[c] * keep + 127) / 255);
                }
            }
        }
    }
}

void DeskView::hide() {
    if (buttons_held_ > 0 || GetCapture() == hwnd_) {
        ReleaseCapture();
        buttons_held_ = 0;
    }
    input_.release_all();
    if (bits_) {
        const size_t pitch = static_cast<size_t>(screen_.right - screen_.left) * 4;
        for (const auto& p : placed_) {
            for (int y = p.rect.top; y < p.rect.bottom; ++y) {
                std::memset(bits_ + y * pitch + static_cast<size_t>(p.rect.left) * 4, 0,
                            static_cast<size_t>(p.rect.right - p.rect.left) * 4);
            }
        }
    }
    placed_.clear();
    first_update_ = true;
    if (visible_) {
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
    }
}

NormPoint DeskView::norm(LPARAM lp) const {
    // Client coordinates = screen coordinates of the primary monitor (the window covers it from 0,0).
    const double x = GET_X_LPARAM(lp) + screen_.left;
    const double y = GET_Y_LPARAM(lp) + screen_.top;
    return {layout_.desk_w > 0 ? x / layout_.desk_w : 0.0, layout_.desk_h > 0 ? y / layout_.desk_h : 0.0};
}

bool DeskView::on_input(UINT msg, WPARAM wp, LPARAM lp) {
    const auto button_of = [](UINT m, WPARAM w) -> std::optional<std::pair<proto::MouseButton, bool>> {
        switch (m) {
            case WM_LBUTTONDOWN: return std::pair{proto::MouseButton::Left, true};
            case WM_LBUTTONUP: return std::pair{proto::MouseButton::Left, false};
            case WM_RBUTTONDOWN: return std::pair{proto::MouseButton::Right, true};
            case WM_RBUTTONUP: return std::pair{proto::MouseButton::Right, false};
            case WM_MBUTTONDOWN: return std::pair{proto::MouseButton::Middle, true};
            case WM_MBUTTONUP: return std::pair{proto::MouseButton::Middle, false};
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
                return std::pair{GET_XBUTTON_WPARAM(w) == XBUTTON1 ? proto::MouseButton::X1 : proto::MouseButton::X2,
                                 m == WM_XBUTTONDOWN};
            default: return std::nullopt;
        }
    };

    switch (msg) {
        case WM_MOUSEMOVE:
            if (!injected_by_us() && layout_.desk_w > 0) input_.mouse_move(norm(lp));
            return true;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (!injected_by_us()) input_.wheel(GET_WHEEL_DELTA_WPARAM(wp), msg == WM_MOUSEHWHEEL);
            return true;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP:
            // Everything goes to the owner's window, Alt+F4 included (it closes that window, like on one PC).
            if (!injected_by_us()) input_.key(wp, lp, msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
            return true;
        default: break;
    }
    if (const auto b = button_of(msg, wp)) {
        if (injected_by_us() || layout_.desk_w <= 0) return true;
        if (b->second) {
            // Clicking the owner's window (this is on its opaque pixels) brings its layer in front of
            // this PC's own windows, and sends keys typed afterwards to it.
            if (front_ != Front::Owner) {
                front_ = Front::Owner;
                apply_z();
            }
            SetForegroundWindow(hwnd_);
            SetFocus(hwnd_);
            if (buttons_held_++ == 0) SetCapture(hwnd_);
        } else if (buttons_held_ > 0 && --buttons_held_ == 0) {
            ReleaseCapture();
        }
        input_.mouse_button(b->first, b->second, norm(lp));
        return true;
    }
    return false;
}

LRESULT CALLBACK DeskView::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<DeskView*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<DeskView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT DeskView::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case kMsgUpdate:
            update();
            return 0;
        case kMsgSave: {
            const auto& path = *reinterpret_cast<const std::wstring*>(wp);
            if (!bits_) return 0;
            const int w = screen_.right - screen_.left, h = screen_.bottom - screen_.top;
            BITMAPINFOHEADER bh{sizeof(bh)};
            bh.biWidth = w;
            bh.biHeight = -h;  // top-down
            bh.biPlanes = 1;
            bh.biBitCount = 32;
            BITMAPFILEHEADER fh{};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(bh);
            fh.bfSize = fh.bfOffBits + static_cast<DWORD>(w) * h * 4;
            FILE* f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return 0;
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&bh, sizeof(bh), 1, f);
            fwrite(bits_, 1, static_cast<size_t>(w) * h * 4, f);
            fclose(f);
            return 1;
        }
        case WM_DISPLAYCHANGE:
            hide();
            if (make_surface()) {
                SetWindowPos(hwnd, nullptr, screen_.left, screen_.top, screen_.right - screen_.left,
                             screen_.bottom - screen_.top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            {
                std::lock_guard lock(pending_mutex_);
                pending_layout_ = layout_;  // draw everything again on the new surface
            }
            update();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // a click doesn't activate it; on_input decides when it takes the keyboard
        case WM_SETCURSOR:
            SetCursor(cursor_.load());
            return TRUE;
        case WM_CAPTURECHANGED:
            if (buttons_held_ > 0 && reinterpret_cast<HWND>(lp) != hwnd) {
                buttons_held_ = 0;
                input_.release_all();
            }
            return 0;
        case WM_KILLFOCUS:
            input_.release_all();  // no stuck keys on the owner
            return 0;
        case WM_CLOSE:
            return 0;  // only the destructor closes it
        default: break;
    }
    if (on_input(msg, wp, lp)) return 0;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace kvm
