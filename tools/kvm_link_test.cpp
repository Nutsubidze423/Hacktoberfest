// Checks common/kvm_link: both KVM directions on one PC, the guard that keeps
// them exclusive, the "both crossed at once" rule and detach.
//
// The test plays the other PC: it feeds KvmLink the peer's messages and fakes
// this PC's physical input with SendInput (accept_injected). A low-level hook
// swallows every click and key (fake or injected by KvmLink), so nothing
// reaches the desktop; only the cursor moves.
// Usage: kvm_link_test.exe   -- don't touch mouse/keyboard for ~10 s (exit code 0 = all passed)

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kvm/dpi.h"
#include "kvm/kvm_link.h"
#include "kvm/win32_input.h"

namespace display = kvm::display;
namespace proto = kvm::proto;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) {
        ++g_failures;
    }
}

std::mutex g_mu;
std::vector<proto::Message> g_sent;  // what this PC sent to the "peer"
int g_keys_seen = 0;                 // keys injected here by KvmLink (swallowed)

bool sink(const proto::Message& m) {
    std::lock_guard l(g_mu);
    g_sent.push_back(m);
    return true;
}

std::vector<proto::Message> take_sent() {
    std::lock_guard l(g_mu);
    return std::exchange(g_sent, {});
}

template <class M>
int count(const std::vector<proto::Message>& msgs) {
    int n = 0;
    for (const auto& m : msgs) n += std::holds_alternative<M>(m);
    return n;
}

LRESULT CALLBACK swallow_mouse(int code, WPARAM wp, LPARAM lp) {
    const auto& m = *reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
    if (code == HC_ACTION && wp != WM_MOUSEMOVE && (m.flags & LLMHF_INJECTED)) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK swallow_key(int code, WPARAM wp, LPARAM lp) {
    const auto& k = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
    if (code == HC_ACTION && (k.flags & LLKHF_INJECTED)) {
        if (kvm::input::is_own_injection(k.dwExtraInfo)) {
            std::lock_guard l(g_mu);
            ++g_keys_seen;
        }
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void abs_move(display::Point p) {
    const auto vd = display::virtual_desktop();
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = static_cast<LONG>((2LL * (p.x - vd.left) + 1) * 32768 / vd.width());
    in.mi.dy = static_cast<LONG>((2LL * (p.y - vd.top) + 1) * 32768 / vd.height());
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof(in));
}

bool wait_until(const std::function<bool()>& cond, int ms = 1500) {
    for (int i = 0; i < ms / 5; ++i) {
        if (cond()) return true;
        Sleep(5);
    }
    return cond();
}

display::Point cursor() {
    POINT c{};
    GetCursorPos(&c);
    return {c.x, c.y};
}

bool cursor_at(display::Point p) {
    const auto c = cursor();
    return c.x == p.x && c.y == p.y;
}

kvm::KvmOptions options(bool is_host) {
    kvm::KvmOptions o;
    o.peer_side = display::Side::Right;
    o.is_host = is_host;
    o.capture.switch_cooldown_ms = 0;
    o.capture.accept_injected = true;
    o.leave_cooldown = std::chrono::milliseconds(0);
    return o;
}

}  // namespace

int main() {
    std::thread([] { Sleep(40000); std::printf("\nwatchdog: test hung, exiting\n"); TerminateProcess(GetCurrentProcess(), 3); }).detach();
    check(display::enable_dpi_awareness(), "dpi awareness");
    const auto orig = cursor();

    std::atomic<DWORD> tid{0};
    std::thread swallower([&] {
        HHOOK hm = SetWindowsHookExW(WH_MOUSE_LL, swallow_mouse, GetModuleHandleW(nullptr), 0);
        HHOOK hk = SetWindowsHookExW(WH_KEYBOARD_LL, swallow_key, GetModuleHandleW(nullptr), 0);
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
        tid = GetCurrentThreadId();
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
        UnhookWindowsHookEx(hm);
        UnhookWindowsHookEx(hk);
    });
    while (!tid) Sleep(1);

    const auto mons = display::monitors();
    const display::OuterEdge right(mons, display::Side::Right);
    const auto& r = mons.front().rect;
    const display::Point inside{(r.left + r.right) / 2, (r.top + r.bottom) / 2};

    {
        kvm::KvmLink kvm(options(/*is_host=*/true));
        check(kvm.start(), "start(): hooks installed");
        kvm.attach(sink);

        // -- the peer takes control of this PC
        check(kvm.handle(proto::FocusEnter{.y_norm = 0.5}), "focus_enter is KVM");
        check(kvm.controlled_by_peer() && !kvm.controlling_peer(), "peer controls this PC");
        const auto entry = display::inward(right.point(0.5), display::Side::Right, 2);
        check(wait_until([&] { return cursor_at(entry); }), "cursor placed 2 px inside the edge facing the peer");

        // -- guard: our own edge can't hand our input over meanwhile
        abs_move(inside); Sleep(60);
        abs_move(right.point(0.5)); Sleep(120);
        check(!kvm.controlling_peer(), "while controlled: this PC's edge does not switch");
        check(count<proto::FocusEnter>(take_sent()) == 0, "...and no focus_enter is sent");

        // -- the peer's input is injected here
        check(kvm.handle(proto::Key{.vk = VK_F13, .scan = 0x64, .down = true}), "key from the peer is KVM while controlled");
        check(kvm.handle(proto::MouseButtonEvent{.button = proto::MouseButton::Left, .down = false}),
              "button from the peer is KVM while controlled");
        Sleep(60);
        check(g_keys_seen == 1, "key injected here");

        // -- the peer's cursor goes back through our edge
        kvm.handle(proto::MouseMove{.dx = 100000, .dy = 0});
        wait_until([&] { return right.position(cursor()).has_value(); });
        kvm.handle(proto::MouseMove{.dx = 3, .dy = 0});
        check(!kvm.controlled_by_peer(), "peer's cursor left through our edge -> no longer controlled");
        check(count<proto::FocusLeave>(take_sent()) == 1, "focus_leave sent to the peer");
        Sleep(60);
        check(g_keys_seen == 2, "held key released when control left");

        // -- now this PC can control the peer
        abs_move(inside); Sleep(60);
        abs_move(right.point(0.3));
        check(wait_until([&] { return kvm.controlling_peer(); }), "guard lifted: our edge -> we control the peer");
        Sleep(60);
        check(count<proto::FocusEnter>(take_sent()) == 1, "focus_enter sent to the peer");

        // -- while we control the peer, its focus_leave means "your cursor is back"
        kvm.handle(proto::FocusLeave{.y_norm = 0.6});
        check(wait_until([&] { return !kvm.controlling_peer(); }), "focus_leave while controlling -> control back here");
        const auto back = display::inward(right.point(0.6), display::Side::Right, 2);
        check(wait_until([&] { return cursor_at(back); }), "cursor returns at the peer's crossing point");

        // -- viewer-style input while nobody controls this PC is not KVM
        check(!kvm.handle(proto::Key{.vk = VK_F13, .scan = 0x64, .down = true}), "key while not controlled -> not KVM");
        check(!kvm.handle(proto::MouseButtonEvent{.button = proto::MouseButton::Left, .down = true}),
              "button while not controlled -> not KVM");
        check(!kvm.handle(proto::MouseWheel{.delta = 120}), "wheel while not controlled -> not KVM");
        check(kvm.handle(proto::MouseMove{.dx = 5, .dy = 0}), "mouse_move is always KVM (ignored here)");
        check(!kvm.handle(proto::Ping{}) && !kvm.handle(proto::Hello{}), "other messages -> not KVM");

        // -- the peer takes its input back (its emergency exit / own switch)
        kvm.handle(proto::FocusEnter{.y_norm = 0.5});
        kvm.handle(proto::FocusLeave{.y_norm = 0.5});
        check(!kvm.controlled_by_peer(), "peer's focus_leave while controlled -> released");
        abs_move(inside); Sleep(60);
        abs_move(right.point(0.5));
        check(wait_until([&] { return kvm.controlling_peer(); }), "...and our edge works again");
        take_sent();

        // -- both cross at once, this PC is the host: it keeps control
        check(kvm.handle(proto::FocusEnter{.y_norm = 0.5}), "peer's focus_enter while we control it");
        Sleep(60);
        check(kvm.controlling_peer() && !kvm.controlled_by_peer(), "host keeps control, peer's switch ignored");
        kvm.handle(proto::FocusLeave{.y_norm = 0.5});
        wait_until([&] { return !kvm.controlling_peer(); });

        // -- detach while controlled releases everything
        kvm.handle(proto::FocusEnter{.y_norm = 0.5});
        kvm.handle(proto::Key{.vk = VK_F14, .scan = 0x65, .down = true});
        Sleep(60);
        const int before = g_keys_seen;
        kvm.detach();
        Sleep(60);
        check(!kvm.controlled_by_peer() && g_keys_seen == before + 1, "detach() releases held keys");
    }

    // -- both cross at once, this PC is the client: it yields to the host
    {
        kvm::KvmLink kvm(options(/*is_host=*/false));
        check(kvm.start(), "client: start()");
        kvm.attach(sink);
        abs_move(inside); Sleep(60);
        abs_move(right.point(0.5));
        check(wait_until([&] { return kvm.controlling_peer(); }), "client controls the host");
        take_sent();
        kvm.handle(proto::FocusEnter{.y_norm = 0.5});
        check(wait_until([&] { return !kvm.controlling_peer(); }), "host's focus_enter wins: client gives control back");
        check(kvm.controlled_by_peer(), "...and is controlled by the host");
        Sleep(60);
        check(count<proto::FocusLeave>(take_sent()) == 0, "...without a focus_leave the host would misread");
    }

    SetCursorPos(orig.x, orig.y);
    PostThreadMessageW(tid, WM_QUIT, 0, 0);
    swallower.join();
    std::printf("\n%s\n", g_failures == 0 ? "all passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
