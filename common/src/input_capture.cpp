#include "kvm/input_capture.h"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <cstdlib>
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
constexpr UINT kMsgCooldown = WM_APP + 6;  // suspension ended
constexpr UINT kMsgSuspended = WM_APP + 7; // suspension started

// While the peer controls this PC, this much of our own mouse movement within
// kTakeoverWindowMs means "I want my PC back" (touchpad noise stays below it).
constexpr int kTakeoverPx = 24;
constexpr ULONGLONG kTakeoverWindowMs = 300;

// When control comes back, the cursor lands this far inside A's edge so the
// next small movement does not bounce it straight back to B.
constexpr int kReturnInset = 2;

// Hook moves seen on B without any Raw Input motion before motion is taken
// from the hook instead (devices whose movement never shows up in Raw Input).
constexpr int kRawGrace = 12;

// Mouse moves go out at most this often (merged in between): a 1000 Hz mouse
// would otherwise flood Wi-Fi with tiny packets. Other input is never delayed.
constexpr auto kMoveInterval = std::chrono::milliseconds(4);

constexpr UINT_PTR kMonitorTimer = 1;
constexpr UINT kMonitorRefreshMs = 2000;

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
    // Per stay on B:
    static inline input::Point exit_point;         // where the cursor left A
    static inline int hook_moves = 0;
    static inline int raw_moves = 0;
    static inline bool hook_motion = false;        // motion from the hook (cursor parked) instead of Raw Input
    static inline input::Point park;
    static inline ULONGLONG no_switch_until = 0;   // GetTickCount64() of the end of the cooldown
    static inline bool drag_signalled = false;     // this left-button drag already reported at the edge
    static constexpr unsigned kLeftButton = 1u << static_cast<unsigned>(proto::MouseButton::Left);

    // Left button held while the cursor touches the edge: e.g. files dragged
    // from Explorer that should go to the peer. Reported once per touch.
    // Own (physical) mouse movement while the peer controls this PC.
    static inline int takeover_px = 0;
    static inline ULONGLONG takeover_since = 0;
    static inline bool takeover_signalled = false;

    static void reset_takeover() {
        takeover_px = 0;
        takeover_since = 0;
        takeover_signalled = false;
    }

    static void local_move_while_suspended(const POINT& pt) {
        if (takeover_signalled) return;
        // The cursor hasn't moved yet when the hook runs, so this is purely our own
        // movement, whatever the peer's injected input did in between.
        POINT cur{};
        GetCursorPos(&cur);
        const ULONGLONG now = GetTickCount64();
        if (now - takeover_since > kTakeoverWindowMs) {
            takeover_px = 0;
            takeover_since = now;
        }
        takeover_px += std::abs(pt.x - cur.x) + std::abs(pt.y - cur.y);
        if (takeover_px < kTakeoverPx) return;
        takeover_signalled = true;
        cap->events_.push({.msg = proto::FocusLeave{}, .send = false, .local_activity = true});
    }

    static void drag_at_edge(double y_norm, input::Point at) {
        if (drag_signalled || cap->disabled_ || !cap->has_client_ || cap->suspended_) return;
        drag_signalled = true;
        cap->events_.push({.msg = proto::FocusEnter{.y_norm = y_norm}, .send = false, .drag_at = at});
    }

    static void start_cooldown() {
        no_switch_until = GetTickCount64() + static_cast<ULONGLONG>(std::max(0, cap->options_.switch_cooldown_ms));
    }

    static void push(proto::Message msg, bool send = true, bool emergency = false) {
        cap->events_.push({std::move(msg), send, emergency});
    }

    static void note(const char* text) {
        cap->events_.push({proto::MouseMove{}, false, false, text});
    }

    static input::Point cursor_pos() {
        POINT pt{};
        GetCursorPos(&pt);
        return {pt.x, pt.y};
    }

    // Parks A's cursor in the middle of its monitor; from now on each hook move
    // is measured from there (and swallowed, so the cursor stays put).
    static void start_hook_motion() {
        hook_motion = true;
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        const POINT at{exit_point.x, exit_point.y};
        GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi);
        park = {(mi.rcMonitor.left + mi.rcMonitor.right) / 2, (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2};
        SetCursorPos(park.x, park.y);
        if (cap->options_.raw_input) note("input capture: no Raw Input motion from this device, taking motion from the hook");
    }

    static void refresh_monitors() {
        edge = display::OuterEdge(display::monitors(), cap->options_.client_side);
    }

    static bool is_physical(ULONG_PTR extra, bool injected) {
        if (input::is_own_injection(extra)) return false;
        return !injected || cap->options_.accept_injected;
    }

    // Returns true if control moved to B.
    static bool enter_remote(double y_norm, input::Point at) {
        if (cap->remote_ || cap->disabled_ || !cap->has_client_ || cap->suspended_) return false;
        cap->remote_ = true;
        keys_pass_up = keys_down;
        have_abs = false;
        exit_point = at;
        hook_moves = raw_moves = 0;
        hook_motion = false;
        push(proto::FocusEnter{.y_norm = y_norm});
        if (!cap->options_.raw_input) start_hook_motion();
        return true;
    }

    // A takes control back on its own (switch_to_local, client gone, send failed):
    // the cursor goes back to where it left, 2 px inside the edge.
    static void leave_remote(bool tell_b) {
        if (!cap->remote_) return;
        cap->remote_ = false;
        const auto on_edge = edge.position(exit_point);
        const auto back = on_edge ? display::inward(exit_point, edge.side(), kReturnInset) : exit_point;
        SetCursorPos(back.x, back.y);
        start_cooldown();
        push(proto::FocusLeave{.y_norm = on_edge.value_or(0.5)}, tell_b);
    }

    // B's cursor left through the edge facing A at y_norm.
    static void returned(double y_norm) {
        if (!cap->remote_) return;
        cap->remote_ = false;
        const auto in = display::inward(edge.point(y_norm), edge.side(), kReturnInset);
        SetCursorPos(in.x, in.y);  // not SendInput: never feed input through our own hook
        start_cooldown();
        push(proto::FocusLeave{.y_norm = y_norm}, /*send=*/false);
    }

    static void emergency() {
        if (cap->remote_) {
            const auto back = edge.position(exit_point) ? display::inward(exit_point, edge.side(), kReturnInset) : exit_point;
            SetCursorPos(back.x, back.y);
        }
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
            if (wp == WM_MOUSEMOVE && cap->options_.edge_switching) {
                const auto t = edge.position({m.pt.x, m.pt.y});
                if (t && buttons_down == 0 && GetTickCount64() >= no_switch_until) {
                    // With the cursor parked, this move must not land on the edge.
                    if (enter_remote(*t, {m.pt.x, m.pt.y}) && hook_motion) return 1;
                } else if (t && buttons_down == kLeftButton) {
                    drag_at_edge(*t, {m.pt.x, m.pt.y});  // never switch mid-drag, but a file drag may want out
                } else if (!t) {
                    drag_signalled = false;
                }
            }
            if (!(buttons_down & kLeftButton)) drag_signalled = false;
            if (wp == WM_MOUSEMOVE && cap->suspended_) local_move_while_suspended(m.pt);
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        // Control is on B: swallow everything, forward what matters. Motion
        // normally comes from Raw Input (the hook only sees the clipped cursor
        // position at the edge); see start_hook_motion() for the fallback.
        if (wp == WM_MOUSEMOVE) {
            ++hook_moves;
            if (hook_motion) {
                const int dx = m.pt.x - park.x;
                const int dy = m.pt.y - park.y;
                if (dx != 0 || dy != 0) push(proto::MouseMove{.dx = dx, .dy = dy});
            } else if (raw_moves == 0 && hook_moves >= kRawGrace) {
                start_hook_motion();
            }
        } else if (button) {
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
        if (raw.header.dwType != RIM_TYPEMOUSE || !cap->remote_ || hook_motion) return;
        const RAWMOUSE& rm = raw.data.mouse;
        // Only our own injected moves are skipped. A null hDevice does NOT mean
        // injected: precision touchpads deliver all their motion that way.
        if (input::is_own_injection(rm.ulExtraInformation)) return;

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
        if (dx != 0 || dy != 0) {
            ++raw_moves;
            push(proto::MouseMove{.dx = dx, .dy = dy});
        }
    }

    static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_INPUT:
                raw_input(reinterpret_cast<HRAWINPUT>(lp));
                return DefWindowProcW(h, msg, wp, lp);  // required for RIM_INPUT cleanup
            case WM_DISPLAYCHANGE:
                refresh_monitors();
                return 0;
            case WM_TIMER:
                if (wp == kMonitorTimer) refresh_monitors();
                return 0;
            case kMsgRemote: enter_remote(unpack(wp), cursor_pos()); return 0;
            case kMsgLocal: leave_remote(wp != 0); return 0;
            case kMsgReturned: returned(unpack(wp)); return 0;
            case kMsgUnhook: unhook(); return 0;
            case kMsgCooldown: start_cooldown(); return 0;
            case kMsgSuspended: reset_takeover(); return 0;
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
    log::info("input capture: hooks installed, the other PC is on the {}{}. Emergency exit: Ctrl+Alt+Shift+Q",
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

void InputCapture::switch_to_local(bool tell_peer) {
    post(kMsgLocal, tell_peer ? 1 : 0);
}

void InputCapture::set_on_drag_at_edge(DragAtEdge callback) {
    std::lock_guard lock(sink_mutex_);
    on_drag_at_edge_ = std::move(callback);
}

void InputCapture::set_suspended(bool suspended) {
    const bool was = suspended_.exchange(suspended);
    if (was && !suspended) post(kMsgCooldown);
    if (!was && suspended) post(kMsgSuspended);
}

void InputCapture::set_on_local_activity(LocalActivity callback) {
    std::lock_guard lock(sink_mutex_);
    on_local_activity_ = std::move(callback);
}

bool InputCapture::post(unsigned msg, unsigned long long wparam) {
    HWND h = static_cast<HWND>(hwnd_.load());
    return h && PostMessageW(h, msg, static_cast<WPARAM>(wparam), 0);
}

void InputCapture::hook_thread_main() {
    using T = CaptureThread;
    // Input latency matters more than anything else this thread could wait for,
    // and a hook thread that doesn't answer in time gets unhooked by Windows.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    T::cap = this;
    T::no_switch_until = 0;
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
    // Also re-read the monitors now and then: a virtual display (VDD) appearing
    // or moving must never leave the edge stale, even if WM_DISPLAYCHANGE is missed.
    SetTimer(T::hwnd, kMonitorTimer, kMonitorRefreshMs, nullptr);
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
    using Clock = std::chrono::steady_clock;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    std::vector<Event> batch;
    bool send_failed = false;  // warn once per stay on B
    struct {
        int moves = 0, buttons = 0, wheels = 0, keys = 0;
    } sent;  // per stay on B, for the log
    Clock::time_point last_move_sent{};

    const auto is_move = [](const Event& ev) { return !ev.note && std::holds_alternative<proto::MouseMove>(ev.msg); };
    // Appends ev, merging consecutive mouse moves into one message.
    const auto append = [&](Event&& ev) {
        if (!batch.empty() && is_move(batch.back()) && is_move(ev)) {
            auto& prev = std::get<proto::MouseMove>(batch.back().msg);
            const auto& next = std::get<proto::MouseMove>(ev.msg);
            prev.dx += next.dx;
            prev.dy += next.dy;
        } else {
            batch.push_back(std::move(ev));
        }
    };

    while (auto first = events_.pop()) {
        batch.clear();
        append(std::move(*first));
        while (auto more = events_.try_pop()) append(std::move(*more));
        // Pace motion (kMoveInterval); anything that isn't a move flushes right away.
        while (is_move(batch.back()) && Clock::now() < last_move_sent + kMoveInterval) {
            auto more = events_.pop_for(last_move_sent + kMoveInterval - Clock::now());
            if (!more) break;
            const bool move = is_move(*more);
            append(std::move(*more));
            if (!move) break;
        }

        for (const Event& ev : batch) {
            if (ev.note) {
                log::info("{}", ev.note);
                continue;
            }
            if (ev.drag_at) {
                std::lock_guard lock(sink_mutex_);
                if (on_drag_at_edge_) on_drag_at_edge_(std::get<proto::FocusEnter>(ev.msg).y_norm, *ev.drag_at);
                continue;
            }
            if (ev.local_activity) {
                LocalActivity callback;
                {
                    std::lock_guard lock(sink_mutex_);
                    callback = on_local_activity_;
                }
                if (callback) callback();  // outside the lock: it may send to the peer
                continue;
            }
            if (const auto* enter = std::get_if<proto::FocusEnter>(&ev.msg)) {
                send_failed = false;
                sent = {};
                log::info("input capture: control -> other PC (edge at {:.3f})", enter->y_norm);
            } else if (const auto* leave = std::get_if<proto::FocusLeave>(&ev.msg)) {
                if (ev.emergency) {
                    log::warn("input capture: Ctrl+Alt+Shift+Q - control back here, hooks removed (restart to use the KVM)");
                } else {
                    log::info("input capture: control back here (edge at {:.3f})", leave->y_norm);
                }
                log::info("input capture: sent to the other PC: {} moves, {} buttons, {} wheel, {} keys", sent.moves, sent.buttons,
                          sent.wheels, sent.keys);
            }
            if (!ev.send) continue;
            std::lock_guard lock(sink_mutex_);
            if (!sink_) continue;
            if (sink_(ev.msg)) {
                if (std::holds_alternative<proto::MouseMove>(ev.msg)) {
                    ++sent.moves;
                    last_move_sent = Clock::now();
                }
                if (std::holds_alternative<proto::MouseButtonEvent>(ev.msg)) ++sent.buttons;
                if (std::holds_alternative<proto::MouseWheel>(ev.msg)) ++sent.wheels;
                if (std::holds_alternative<proto::Key>(ev.msg)) ++sent.keys;
            } else {
                // The connection is gone: don't keep handing control to it until
                // set_client() attaches a new one (the session may take a while to end).
                has_client_ = false;
                if (remote_ && !send_failed) {
                    send_failed = true;
                    log::warn("input capture: send to the other PC failed, control back here");
                    post(kMsgLocal, 0);
                }
            }
        }
    }
}

}  // namespace kvm::host
