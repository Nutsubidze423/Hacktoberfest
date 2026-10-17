#pragma once
// KVM on A: low-level mouse/keyboard hooks that hand control of the physical
// mouse and keyboard to B.
//
//   kvm::host::InputCapture capture({.client_side = Side::Right});
//   capture.start();
//   capture.set_client([&](const proto::Message& m) { return conn.send(m); });  // on connect
//   ...  B sends focus_leave  ->  capture.on_focus_leave(msg);
//   capture.set_client(nullptr);                                             // on disconnect
//
// Local mode: input goes to A as usual. When the cursor hits the outer edge on
// client_side (and a client is attached, no mouse button held), control moves
// to B: focus_enter is sent, A's local input is swallowed, and mouse motion
// (from Raw Input), buttons, wheel and keys are sent to B instead.
// Control comes back when B sends focus_leave, the client goes away, a send
// fails, or on the emergency exit.
//
// Ctrl+Alt+Shift+Q always works: control returns to A and the hooks are removed
// for good (restart the program to use the KVM again).

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include "kvm/dpi.h"
#include "kvm/protocol.h"
#include "kvm/queue.h"

namespace kvm::host {

struct CaptureOptions {
    // Where B stands relative to A (config.json client_side).
    display::Side client_side = display::Side::Right;
    // Switch to B when the cursor hits that edge. false = only switch_to_remote().
    bool edge_switching = true;
    // After control comes back to A, the edge ignores the cursor this long, so a
    // jittery hand or touchpad doesn't bounce straight back to B.
    int switch_cooldown_ms = 250;
    // Test only: treat input injected by other programs as physical input.
    // Our own injected events (kvm::input::kInjectedMarker) are always ignored.
    bool accept_injected = false;
    // Take motion from Raw Input (unaccelerated). If Raw Input delivers no motion
    // while on B (or this is false), motion comes from the hook instead, with A's
    // cursor parked in the middle of its monitor.
    bool raw_input = true;
};

class InputCapture {
public:
    // Sends one message to B; false = the connection is gone.
    // Called on the capture's worker thread, never from a hook callback.
    using Sink = std::function<bool(const proto::Message&)>;

    explicit InputCapture(CaptureOptions options);
    ~InputCapture();  // stop()

    InputCapture(const InputCapture&) = delete;
    InputCapture& operator=(const InputCapture&) = delete;

    // Installs the hooks on a dedicated thread. Only one InputCapture may run per
    // process. Returns false if the hooks could not be installed.
    bool start();
    // Returns control to A, removes the hooks and joins the threads. Idempotent.
    void stop();

    // The client to send to; nullptr when there is none (control returns to A).
    // Blocks until the previous sink is no longer in use.
    void set_client(Sink sink);

    // B reports that its cursor left through the edge facing A at y_norm.
    void on_focus_leave(const proto::FocusLeave& msg);

    // Hand control to B / take it back from code (e.g. a hotkey or a test).
    void switch_to_remote(double y_norm = 0.5);
    void switch_to_local();

    // Control is currently on B.
    bool remote() const { return remote_; }
    // The emergency exit was used; the hooks are gone.
    bool disabled() const { return disabled_; }

private:
    friend struct CaptureThread;  // hook callbacks and window procedure (input_capture.cpp)

    struct Event {
        proto::Message msg;
        bool send = true;            // false = only logged (e.g. B's own focus_leave)
        bool emergency = false;      // FocusLeave caused by Ctrl+Alt+Shift+Q
        const char* note = nullptr;  // log line only, msg is ignored
    };

    void hook_thread_main();
    void worker_main();
    bool post(unsigned msg, unsigned long long wparam = 0);

    CaptureOptions options_;
    std::atomic<bool> remote_{false};
    std::atomic<bool> disabled_{false};
    std::atomic<bool> has_client_{false};
    std::atomic<void*> hwnd_{nullptr};  // hook thread's hidden window
    std::atomic<int> started_{0};       // 0 = starting, 1 = running, -1 = failed

    BlockingQueue<Event> events_{4096};
    std::mutex sink_mutex_;
    Sink sink_;

    std::thread hook_thread_;
    std::thread worker_;
};

}  // namespace kvm::host
