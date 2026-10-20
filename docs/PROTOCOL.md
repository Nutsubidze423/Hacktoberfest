# Sarke wire protocol

Protocol version **3** (`kProtocolVersion` in [protocol.h](../common/include/kvm/protocol.h)). The
code is the authority: `protocol.h` defines every message, and this page explains them. Change the
two together, and bump the version when an older peer can't understand the change.

**Version history:**
- **3:** the control channel is TLS 1.3 between paired PCs, and `hello` carries `device_id`.
  Version 2 peers can't connect (they don't speak TLS).
- **2:** the hackathon release, Sarke 1.1.0.

## Connections

`kvm_host` listens and `kvm_client` connects. Once connected the roles don't matter: either side
can take control, share windows, copy and send files. Every PC starts as a host; the launcher
(`sarke.exe`) finds the others on the network and connects to the one the user picks.

| Port | Transport | Framing | Carries |
|---|---|---|---|
| 50500 | TCP, **TLS 1.3** | `[u32 len][JSON]` inside TLS, big-endian length, at most 64 KB | control and input messages (table below) |
| 50501 | TCP | `[u32 header_len][u32 payload_len][header JSON][payload]`, big-endian, payload at most 32 MB | video frames, both directions on one connection |
| 50502 | TCP | same as 50501 | files: `transfer_begin` (`drag`, drag image), `file_begin`, `file_chunk`, `file_end`, `transfer_end` |
| 50503 | UDP | one JSON object per datagram | discovery: `kvm_discover` (broadcast), answered by `kvm_here` |

`TCP_NODELAY` is set on every TCP socket.

**Who may connect** (50500; [SECURITY.md](SECURITY.md) §6.4):
- Both sides present a certificate. The TLS handshake runs first.
- Then the peer's device ID (SHA-256 of its key) must be in this PC's `paired.json`. Otherwise the
  connection is closed **before anything is read from it**.
- A peer that doesn't speak TLS (protocol v2) is closed at the handshake.

Until roadmap task P1.6 adds pairing by code, `--pair-with <device id>` pairs two PCs by hand
(`kvm_host --print-id` shows the ID). Ports 50501 and 50502 are still plain TCP until P1.7.

**Discovery.** A PC looking for others broadcasts `kvm_discover`. Every running host answers
`kvm_here` with `name`, `control_port`, `protocol_version` and `busy` (a client is already
connected). The launcher scans every 2.5 s and lists the PCs in the tray ("Connect to ▸"). It
connects by itself only to PCs marked "Connect automatically" (`settings.json` `auto_connect`).
`config.json` `"host_ip": "auto"` (the default) needs no IP; `kvm_client --host <IP>` connects
directly.

## Control messages (port 50500)

Every message is a JSON object with a `type` field. Use `encode()` / `decode()` from `protocol.h`,
never hand-built JSON.

**Roles in the Direction column:**
- **controller / controlled:** in KVM, the PC whose mouse and keyboard are in use, and the PC they
  are currently driving.
- **window owner / viewer:** the PC where a shared window's process runs, and the PC that shows it.

| `type` | Direction | Fields |
|---|---|---|
| `hello` | client → host, then host → client (the answer) | `protocol_version`, `screen_w`, `screen_h`, `dpi_scale`, `hostname`, `desk` (desk mode on), `device_id` (64 hex, the sender's TLS key; for logs) |
| `focus_enter` | controller → controlled | `y_norm` (where the cursor enters), `desk` (the cursor follows `desk_move`) |
| `focus_leave` | both | `y_norm`. Controller → controlled: "I took input back". Controlled → controller: "your cursor came back over my edge" |
| `mouse_move` | controller → controlled | `dx`, `dy` (relative) |
| `mouse_button` | both | `button` (`left`/`right`/`middle`/`x1`/`x2`), `down`. Viewer → window owner adds `x_norm`, `y_norm` |
| `mouse_wheel` | both | `delta`, `horizontal` |
| `key` | both | `vk`, `scan`, `down`, `extended` |
| `desk_move` | controller → controlled (desk mode) | `x`, `y`: the cursor on the controlled PC's primary monitor, in px (that is, the controller's VDD) |
| `desk_cursor` | window owner → viewer | `shape` (`arrow`/`ibeam`/`hand`/`sizewe`/…): the cursor shape over the owner's windows |
| `desk_layout` | window owner → viewer | `desk_w`, `desk_h`, `active` (the owner's active window is on the VDD, so the viewer raises its overlay), `windows` [`id`, `x`, `y`, `w`, `h`] (the windows on the VDD, top to bottom). Moving a window sends only this message |
| `stream_start` | window owner → viewer | `hwnd`, `title`, `width`, `height` |
| `stream_stop` | both | `hwnd`. Owner → viewer: the stream stopped. Viewer → owner: "take it back" |
| `remote_mouse_move` | viewer → window owner | `x_norm`, `y_norm` (in the streamed window, 0..1; the owner maps them onto the window's current client area) |
| `clipboard` | both | `seq`, `kind` (`text` UTF-8, `png`, or `files`: the copied files' names, with the files going over 50502), `total`, `offset`, `data` (base64, at most 32 KB per message; one copy in several parts) |
| `layout` | both (after `hello`, and on Displays → Apply) | `you_are` (`left`/`right`/`top`/`bottom`: where the **receiver** stands relative to the sender), `set_at` (ms when the user set it; 0 = never). The newer setting wins; on a tie, the host's. Each PC keeps every peer's side in `config.json` `"peers": {name: {side, set_at}}` ([layout_sync](../common/include/kvm/layout_sync.h)) |
| `identify` | both | none. The other PC shows its screen numbers too |
| `ping` / `pong` | both | `t` (`pong` adds `t_peer`, for the clock offset) |

## Video frames (port 50501)

`frame` header, window owner → viewer:

| Field | Meaning |
|---|---|
| `seq` | frame number, gap-free per stream |
| `t_capture_us` | when it was captured (owner's clock; the viewer maps it with the ping/pong offset) |
| `width`, `height` | picture size |
| `codec` | `jpeg` |
| `layer` | `"desk"` in desk mode |
| `window` | desk mode: the window this picture is of (one window per frame, WGC window capture) |
| `x`, `y` | desk mode: where the picture sits in the window's frame |
| `alpha_bytes` | desk mode: the payload is the JPEG (4:4:4) followed by this many bytes of alpha runs `[u8 alpha][u16 count]` (rounded corners) |

The sender always sends the newest frame and drops older ones; it never queues video.
