Handoff · Demetre → Luka and his Claude · 17.10.2026

# KVM both ways (B controls A too) + shared clipboard

Claude, this file is for you. Demetre's side of two new features is on `main` and tested (`0450c68`, `92aff9a`). To make them work between the PCs, Luka's files need: one new protocol message and the wiring in both `main.cpp` files. Everything below is exact code; adapt names if Luka prefers.

## What Demetre added (his files, don't edit them; report bugs to Demetre)

| | What | Test |
| --- | --- | --- |
| `common/kvm_link.*` | `kvm::KvmLink`: runs both KVM halves on one PC (this PC's input → peer, peer's input → here) and routes the peer's messages by state. Only one direction at a time; if both users cross at once, the host keeps control. | `tools/kvm_link_test.exe` |
| `common/input_capture.*`, `common/input_inject.*` | Moved from `host_a/` and `client_b/` so both exes can use both. `host_a/input_capture.h` and `client_b/input_inject.h` forward to them, so today's `main.cpp` still builds. | `input_capture_test`, `input_inject_test` |
| `common/clipboard.*` | `kvm::clip::ClipboardSync`: every copy on one PC goes to the other: text (UTF-8) if any, else an image (PNG). The receiver pastes text as `CF_UNICODETEXT`, images as `CF_DIB` + `"PNG"`. No echo. Content travels as `kvm::clip::Chunk`s of ≤ 32 KB (base64 ≈ 43 KB, fits `kMaxControlMessageBytes`); big copies are spaced out so mouse/keys aren't held up; max 16 MB per copy. Files: not yet (logged). | `tools/clipboard_test.exe` |

Who sends what now (please put this in `protocol.h` comments and the CLAUDE.md table):
- controller → controlled: `focus_enter`, `mouse_move`, `mouse_button`, `mouse_wheel`, `key`, and `focus_leave` meaning "I took my input back" (emergency, disconnect).
- controlled → controller: `focus_leave` meaning "your cursor came back through my edge".
- Either PC can be the controller now. `KvmLink::handle()` tells the meanings apart by its own state.
- `mouse_button` / `mouse_wheel` / `key` from B are KVM input only while B controls A; otherwise `handle()` returns `false` and they are Sandro's viewer input (`remote_input`), as before.

## 1. `common/include/kvm/protocol.h` (Luka only)

```cpp
// Both directions: one piece of copied content (kvm/clipboard.h, kvm::clip::Chunk).
// Pieces of one copy share seq; the receiver joins them by offset until total bytes arrived.
struct Clipboard {
    static constexpr std::string_view kType = "clipboard";
    std::uint32_t seq = 0;
    std::string kind;          // "text" (UTF-8) or "png"
    std::uint32_t total = 0;   // bytes of the whole content
    std::uint32_t offset = 0;  // where this piece starts
    std::string data;          // base64, at most kvm::clip::kChunkBytes (32 KB) decoded
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Clipboard, seq, kind, total, offset, data)
```
- Add `Clipboard` to `using Message = std::variant<...>`.
- Bump `kProtocolVersion` to **2**: both PCs need the new build anyway (B now runs hooks), and a clear "protocol v1 vs v2" beats a half-working KVM.
- `tools/protocol_test.cpp`: `round_trip(proto::Clipboard{.seq = 3, .kind = "text", .total = 5, .offset = 0, .data = "aGVsbG8="});`

## 2. `host_a/main.cpp`

Includes: `#include "kvm/kvm_link.h"` and `#include "kvm/clipboard.h"` instead of `#include "input_capture.h"`.

```cpp
proto::Clipboard to_message(const kvm::clip::Chunk& c) {
    return {.seq = c.seq, .kind = c.kind, .total = c.total, .offset = c.offset, .data = c.data};
}
kvm::clip::Chunk to_chunk(const proto::Clipboard& m) {
    return {.seq = m.seq, .kind = m.kind, .total = m.total, .offset = m.offset, .data = m.data};
}
```

`run_session(..., kvm::KvmLink* kvm, kvm::clip::ClipboardSync* clip, ...)`: replace the current `Detach` struct, `set_client` and the `FocusLeave` branch with:
```cpp
    // KVM + clipboard use conn while the session lasts. detach() blocks until they
    // stop using it, so it must run before conn goes away (struct dtor).
    struct Detach {
        kvm::KvmLink* kvm;
        kvm::clip::ClipboardSync* clip;
        ~Detach() {
            if (clip) clip->detach();
            if (kvm) kvm->detach();
        }
    } detach{kvm, clip};
    if (kvm) kvm->attach([&conn](const proto::Message& m) { return conn.send_json(proto::encode(m)); });
    if (clip) clip->attach([&conn](const kvm::clip::Chunk& c) { return conn.send(to_message(c)); });

    ...
        if (const auto msg = conn.incoming().pop_for(500ms)) {
            if (kvm && kvm->handle(*msg)) {
                // KVM: B's cursor came back, or B controls A
            } else if (const auto* c = std::get_if<proto::Clipboard>(&*msg); c && clip) {
                clip->receive(to_chunk(*c));
            } else {
                // TODO(luka): route to remote_input (sandro) once it exists.
                kvm::log::info("from client: {}", proto::encode(*msg).dump());
            }
        }
```

In `wmain`, instead of `std::optional<kvm::host::InputCapture> capture;` and its `start()`:
```cpp
        std::optional<kvm::KvmLink> kvm_link;
        if (opt.kvm_enabled) {
            kvm_link.emplace(kvm::KvmOptions{.peer_side = *side, .is_host = true});
            kvm_link->start();  // false = hooks failed (logged); B can still control A
        }
        kvm::clip::ClipboardSync clip;
        const bool clip_on = opt.clipboard && clip.start();   // new flag --no-clipboard
        ...
            run_session(*conn, kvm_link ? &*kvm_link : nullptr, clip_on ? &clip : nullptr, video, opt);
```

## 3. `client_b/main.cpp`

Same includes (`kvm_link.h`, `clipboard.h` instead of `input_inject.h`) and the same `to_message` / `to_chunk`.

The `KvmLink` lives for the whole process (its hooks stay installed across reconnects); create it in `wmain` once, after `host_side` is computed:
```cpp
        std::optional<kvm::KvmLink> kvm_link;
        if (kvm_enabled) {   // new flag --no-kvm, as on the host
            kvm_link.emplace(kvm::KvmOptions{.peer_side = host_side, .is_host = false});
            kvm_link->start();  // false = hooks failed (logged); A can still control B
        }
        kvm::clip::ClipboardSync clip;
        const bool clip_on = clipboard_enabled && clip.start();   // --no-clipboard
        ...
                run_session(*conn, cfg, kvm_link ? &*kvm_link : nullptr, clip_on ? &clip : nullptr);
```
`run_session`: drop the `InputInject inject(...)` line; use the same `Detach` struct, `attach` calls and message routing as on the host (`kvm->handle(*msg)` first, then `proto::Clipboard`, then the viewer TODO / log).

## 4. Build and local check

1. `cmake --build --preset x64-release` → zero warnings.
2. `protocol_test`, `kvm_link_test`, `clipboard_test`, `input_capture_test`, `input_inject_test` → all passed.
   - Hands off mouse/keyboard while they run.
   - `clipboard_test` uses the real clipboard and puts the old text back at the end.
3. Loopback: `kvm_host.exe`, then `kvm_client.exe --host 127.0.0.1`. Both logs show `input capture: hooks installed` and `clipboard: sharing text and images`.

## 5. Test 3 on the two PCs (Luka = A/host, Demetre = B/client)

Both PCs must build from the same commit, with protocol v2.

| # | Who | Action | Expected |
| --- | --- | --- | --- |
| 1 | Luka | Push the mouse off A's right edge, use B, come back | Works as in test 2 |
| 2 | Demetre | Push **his touchpad** through B's **left** edge | His cursor appears at A's right edge; he controls A; Luka's mouse still works on A too |
| 3 | Demetre | Type and click on A, then push back through A's right edge | Text appears on A; control returns to B |
| 4 | Both | Both push towards each other at the same moment | Luka (host) ends up controlling B, nothing stuck |
| 5 | Luka | Copy text in Notepad on A, paste on B (Ctrl+V) | Same text, Georgian letters too |
| 6 | Demetre | Copy text on B, paste on A | Same text |
| 7 | Luka | Win+Shift+S screenshot on A, paste into Paint on B | The screenshot |
| 8 | Demetre | Copy a file in Explorer | Log: `clipboard: copied files are not shared yet` (expected for now) |
| 9 | Either | Ctrl+Alt+Shift+Q while controlling the other PC | Control comes back, that PC's hooks are removed |

## Notes

- **Emergency key:** each PC's `Ctrl+Alt+Shift+Q` removes only **that PC's** hooks. Restart that exe to send input again.
- **Elevated windows:** they only accept injected input if the injecting side runs as administrator. This now applies on A too, when B controls A.
- **Security:** keystrokes and clipboard content travel unencrypted. That's fine on a home network, but don't copy passwords across on public Wi-Fi.
- **CLAUDE.md "Structure":**
  - `input_capture.*` and `input_inject.*` now live in `common/`, still Demetre's.
  - Demetre also owns the new `kvm_link.*` and `clipboard.*`.
  - "Clipboard" can leave the "not included" list (text + images; files later, maybe over Temo's proposed file channel).

## What to send back to Demetre

1. The commit with the protocol + `main.cpp` changes.
2. Test 3: pass/fail per row, plus the host/client log lines for anything that failed.
