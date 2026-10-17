#include "input_capture.h"

#include <windows.h>

#include <bit>
#include <bitset>
#include <cstdint>
#include <optional>
#include <vector>

#include "kvm/log.h"
#include "kvm/win32_input.h"

namespace kvm::host {
namespace {

// Posted to the hook thread's window; wparam carries a std::bit_cast'ed y_norm.
constexpr UINT kMsgRemote = WM_APP + 1;    // switch_to_remote
constexpr UINT kMsgLocal = WM_APP + 2;     // A takes control back; wparam 1 = tell B
constexpr UINT kMsgReturned = WM_APP + 3;  // B's focus_leave
constexpr UINT kMsgUnhook = WM_APP + 4;    // after the emergency exit
constexpr UINT kMsgStop = WM_APP + 5;

// When control comes back, the cursor lands this far inside A's edge so the
// next small movement does not bounce it straight back to B.
constexpr int kReturnInset = 2;

constexpr wchar_t kWindowClass[] = L"KvmInputCapture";

WPARAM pack(double y) {
    return static_cast<WPARAM>(std::bit_cast<std::uint64_t>(y));
}

double unpack(WPARAM w) {
    return std::bit_cast<double>(static_cast<std::uint64_t>(w));
}

}  // namespace

// Everything here runs on the hook thread only.
struct CaptureThread {
    static inline InputCapture* cap = nullptr;
    static inline HWND hwnd = nullptr;
    static inline HHOOK mouse_hook = nullptr;
    static inline HHOOK key_hook = nullptr;
    static inline display::OuterEdge edge;
    static inline std::bitset<256> keys_down;      // physical keys held, by vk
    static inline std::bitset<256> keys_pass_up;   // held on A when control left: release goes to A
    static inline unsigned buttons_down = 0;       // physical mouse buttons held (bit per button)
    static inline bool have_abs = false;           // absolute raw input (VMs, tablets, RDP)
    static inline input::Point last_abs;

    static void push(proto::Message msg, bool send = true, bool emergency = false) {
        cap->events_.push({std::move(msg), send, emergency});
    }

    static void refresh_monitors() {
        edge = display::OuterEdge(display::monitors(), cap->options_.client_side);
    }

    static bool is_physical(ULONG_PTR extra, bool injected) {
        if (input::is_own_injection(extra)) return false;
        return !injected || cap->options_.accept_injected;
    }

    static void enter_remote(double y_norm) {
        if (cap->remote_ || cap->disabled_ || !cap->has_client_) return;
        cap->remote_ = true;
        keys_pass_up = keys_down;
        have_abs = false;
        push(proto::FocusEnter{.y_norm = y_norm});
    }

    // A takes control back on its own (switch_to_local, client gone, send failed).
    static void leave_remote(bool tell_b) {
        if (!cap->remote_) return;
        cap->remote_ = false;
        POINT pt{};
        GetCursorPos(&pt);
        const auto on_edge = edge.position({pt.x, pt.y});
        if (on_edge) {
            const auto in = display::inward({pt.x, pt.y}, edge.side(), kReturnInset);
            SetCursorPos(in.x, in.y);
        }
        push(proto::FocusLeave{.y_norm = on_edge.value_or(0.5)}, tell_b);
    }

    // B's cursor left through the edge facing A at y_norm.
    static void returned(double y_norm) {
        if (!cap->remote_) return;
        cap->remote_ = false;
        const auto in = display::inward(edge.point(y_norm), edge.side(), kReturnInset);
        SetCursorPos(in.x, in.y);  // not SendInput: never feed input through our own hook
        push(proto::FocusLeave{.y_norm = y_norm}, /*send=*/false);
    }

    static void emergency() {
        cap->remote_ = false;
        cap->disabled_ = true;
        push(proto::FocusLeave{.y_norm = 0.5}, /*send=*/true, /*emergency=*/true);
        PostMessageW(hwnd, kMsgUnhook, 0, 0);  // unhook outside the callback
    }

    static void unhook() {
        if (mouse_hook) UnhookWindowsHookEx(mouse_hook);
        if (key_hook) UnhookWindowsHookEx(key_hook);
        mouse_hook = key_hook = nullptr;
    }

    static bool ctrl_alt_shift() {
        return (keys_down[VK_LCONTROL] || keys_down[VK_RCONTROL] || keys_down[VK_CONTROL]) &&
               (keys_down[VK_LMENU] || keys_down[VK_RMENU] || keys_down[VK_MENU]) &&
               (keys_down[VK_LSHIFT] || keys_down[VK_RSHIFT] || keys_down[VK_SHIFT]);
    }

    static LRESULT CALLBACK mouse_proc(int code, WPARAM wp, LPARAM lp) {
        if (code != HC_ACTION || !cap) return CallNextHookEx(nullptr, code, wp, lp);
        const auto& m = *reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (!is_physical(m.dwExtraInfo, (m.flags & LLMHF_INJECTED) != 0)) return CallNextHookEx(nullptr, code, wp, lp);

        std::optional<proto::MouseButton> button;
        bool down = false;
        switch (wp) {
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: button = proto::MouseButton::Left; down = wp == WM_LBUTTONDOWN; break;
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: button = proto::MouseButton::Right; down = wp == WM_RBUTTONDOWN; break;
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: button = proto::MouseButton::Middle; down = wp == WM_MBUTTONDOWN; break;
            case WM_XBUTTONDOWN: case WM_XBUTTONUP:
                button = HIWORD(m.mouseData) == XBUTTON1 ? proto::MouseButton::X1 : proto::MouseButton::X2;
                down = wp == WM_XBUTTONDOWN;
                break;
            default: break;
        }
        if (button) {
            const unsigned bit = 1u << static_cast<unsigned>(*button);
            buttons_down = down ? (buttons_down | bit) : (buttons_down & ~bit);
        }

        if (!cap->remote_) {
            // Never switch mid-drag: the button release would be lost on A.
            if (wp == WM_MOUSEMOVE && cap->options_.edge_switching && buttons_down == 0) {
                if (const auto t = edge.position({m.pt.x, m.pt.y})) enter_remote(*t);
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        // Control is on B: swallow everything, forward what matters. Motion
        // comes from Raw Input (the hook only sees the clipped cursor position).
        if (button) {
            push(proto::MouseButtonEvent{.button = *button, .down = down});
        } else if (wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL) {
            push(proto::MouseWheel{.delta = static_cast<short>(HIWORD(m.mouseData)), .horizontal = wp == WM_MOUSEHWHEEL});
        }
        return 1;
    }

    static LRESULT CALLBACK key_proc(int code, WPARAM wp, LPARAM lp) {
        if (code != HC_ACTION || !cap) return CallNextHookEx(nullptr, code, wp, lp);
        const auto& k = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (!is_physical(k.dwExtraInfo, (k.flags & LLKHF_INJECTED) != 0)) return CallNextHookEx(nullptr, code, wp, lp);

        const bool up = (k.flags & LLKHF_UP) != 0;
        const unsigned vk = k.vkCode & 0xFF;

        // Emergency exit, checked before anything else, in every mode.
        if (!up && vk == 'Q' && ctrl_alt_shift()) {
            emergency();
            return 1;
        }
        keys_down[vk] = !up;

        if (!cap->remote_) return CallNextHookEx(nullptr, code, wp, lp);
        if (up && keys_pass_up[vk]) {
            keys_pass_up[vk] = false;
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        // AltGr arrives as a fake LCtrl (scan 0x21D) + RAlt; B's layout makes its own LCtrl.
        if (vk == VK_LCONTROL && (k.scanCode & 0x200)) return 1;

        push(proto::Key{.vk = static_cast<std::uint16_t>(vk),
                        .scan = static_cast<std::uint16_t>(k.scanCode & 0xFF),
                        .down = !up,
                        .extended = (k.flags & LLKHF_EXTENDED) != 0});
        return 1;
    }

    static void raw_input(HRAWINPUT handle) {
        RAWINPUT raw{};
        UINT size = sizeof(raw);
        if (GetRawInputData(handle, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) return;
        if (raw.header.dwType != RIM_TYPEMOUSE || !cap->remote_) return;
        const RAWMOUSE& rm = raw.data.mouse;
        // Injected input has no device handle.
        if (!is_physical(rm.ulExtraInformation, raw.header.hDevice == nullptr)) return;

        int dx = 0, dy = 0;
        if (rm.usFlags & MOUSE_MOVE_ABSOLUTE) {
            const bool virtual_desk = (rm.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
            const display::Rect area = virtual_desk ? display::virtual_desktop()
                                                    : display::Rect{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
            const input::Point p{area.left + static_cast<int>(static_cast<long long>(rm.lLastX) * area.width() / 65536),
                                 area.top + static_cast<int>(static_cast<long long>(rm.lLastY) * area.height() / 65536)};
            if (have_abs) {
                dx = p.x - last_abs.x;
                dy = p.y - last_abs.y;
            }
            last_abs = p;
            have_abs = true;
        } else {
            dx = rm.lLastX;
            dy = rm.lLastY;
        }
        if (dx != 0 || dy != 0) push(proto::MouseMove{.dx = dx, .dy = dy});
    }

    static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_INPUT:
                raw_input(reinterpret_cast<HRAWINPUT>(lp));
                return DefWindowProcW(h, msg, wp, lp);  // required for RIM_INPUT cleanup
            case WM_DISPLAYCHANGE:
                refresh_monitors();
                return 0;
            case kMsgRemote: enter_remote(unpack(wp)); return 0;
            case kMsgLocal: leave_remote(wp != 0); return 0;
            case kMsgReturned: returned(unpack(wp)); return 0;
            case kMsgUnhook: unhook(); return 0;
            case kMsgStop:
                leave_remote(true);
                unhook();
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(h, msg, wp, lp);
        }
    }
};

namespace {
std::atomic<InputCapture*> g_running{nullptr};
}  // namespace

InputCapture::InputCapture(CaptureOptions options) : options_(options) {}

InputCapture::~InputCapture() {
    stop();
}

bool InputCapture::start() {
    InputCapture* expected = nullptr;
    if (!g_running.compare_exchange_strong(expected, this)) {
        log::error("input capture: already running in this process");
        return false;
    }
    started_ = 0;
    hook_thread_ = std::thread(&InputCapture::hook_thread_main, this);
    started_.wait(0);
    if (started_ < 0) {
        hook_thread_.join();
        g_running = nullptr;
        return false;
    }
    worker_ = std::thread(&InputCapture::worker_main, this);
    log::info("input capture: hooks installed, B is on the {}{}. Emergency exit: Ctrl+Alt+Shift+Q",
              display::to_string(options_.client_side), options_.edge_switching ? "" : " (edge switching off)");
    return true;
}

void InputCapture::stop() {
    if (hook_thread_.joinable()) {
        post(kMsgStop);
        hook_thread_.join();
    }
    events_.close();
    if (worker_.joinable()) worker_.join();
    InputCapture* self = this;
    g_running.compare_exchange_strong(self, nullptr);
}

void InputCapture::set_client(Sink sink) {
    {
        std::lock_guard lock(sink_mutex_);
        sink_ = std::move(sink);
        has_client_ = static_cast<bool>(sink_);
    }
    if (!has_client_) post(kMsgLocal, 0);
}

void InputCapture::on_focus_leave(const proto::FocusLeave& msg) {
    post(kMsgReturned, pack(msg.y_norm));
}

void InputCapture::switch_to_remote(double y_norm) {
    post(kMsgRemote, pack(y_norm));
}

void InputCapture::switch_to_local() {
    post(kMsgLocal, 1);
}

bool InputCapture::post(unsigned msg, unsigned long long wparam) {
    HWND h = static_cast<HWND>(hwnd_.load());
    return h && PostMessageW(h, msg, static_cast<WPARAM>(wparam), 0);
}

void InputCapture::hook_thread_main() {
    using T = CaptureThread;
    T::cap = this;
    T::keys_down.reset();
    T::keys_pass_up.reset();
    T::buttons_down = 0;

    auto fail = [&](const char* what) {
        log::error("input capture: {} failed ({})", what, GetLastError());
        T::unhook();
        if (T::hwnd) DestroyWindow(T::hwnd);
        T::hwnd = nullptr;
        T::cap = nullptr;
        started_ = -1;
        started_.notify_all();
    };

    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &T::wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);  // fails harmlessly if already registered by an earlier run
    // Hidden top-level window (message-only windows miss WM_DISPLAYCHANGE broadcasts).
    T::hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!T::hwnd) return fail("CreateWindowEx");

    const RAWINPUTDEVICE rid{.usUsagePage = 0x01, .usUsage = 0x02, .dwFlags = RIDEV_INPUTSINK, .hwndTarget = T::hwnd};
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) return fail("RegisterRawInputDevices");

    T::refresh_monitors();
    T::mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, &T::mouse_proc, inst, 0);
    if (!T::mouse_hook) return fail("SetWindowsHookEx(WH_MOUSE_LL)");
    T::key_hook = SetWindowsHookExW(WH_KEYBOARD_LL, &T::key_proc, inst, 0);
    if (!T::key_hook) return fail("SetWindowsHookEx(WH_KEYBOARD_LL)");

    hwnd_ = T::hwnd;
    started_ = 1;
    started_.notify_all();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);

    T::unhook();
    remote_ = false;
    hwnd_ = nullptr;
    DestroyWindow(T::hwnd);
    T::hwnd = nullptr;
    T::cap = nullptr;
}

void InputCapture::worker_main() {
    std::vector<Event> batch;
    bool send_failed = false;  // warn once per stay on B
    while (auto first = events_.pop()) {
        batch.clear();
        batch.push_back(std::move(*first));
        while (auto more = events_.try_pop()) {
            // Merge consecutive mouse moves into one message.
            auto* prev = std::get_if<proto::MouseMove>(&batch.back().msg);
            const auto* next = std::get_if<proto::MouseMove>(&more->msg);
            if (prev && next) {
                prev->dx += next->dx;
                prev->dy += next->dy;
            } else {
                batch.push_back(std::move(*more));
            }
        }

        for (const Event& ev : batch) {
            if (const auto* enter = std::get_if<proto::FocusEnter>(&ev.msg)) {
                send_failed = false;
                log::info("input capture: control -> B (edge at {:.3f})", enter->y_norm);
            } else if (const auto* leave = std::get_if<proto::FocusLeave>(&ev.msg)) {
                if (ev.emergency) {
                    log::warn("input capture: Ctrl+Alt+Shift+Q - control back on A, hooks removed (restart to use the KVM)");
                } else {
                    log::info("input capture: control -> A (edge at {:.3f})", leave->y_norm);
                }
            }
            if (!ev.send) continue;
            std::lock_guard lock(sink_mutex_);
            if (sink_ && !sink_(ev.msg) && remote_ && !send_failed) {
                send_failed = true;
                log::warn("input capture: send to B failed, control back on A");
                post(kMsgLocal, 0);
            }
        }
    }
}

}  // namespace kvm::host
