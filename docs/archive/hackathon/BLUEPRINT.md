# Sarke — blueprint

Everything the app can do, every control, every setting and every operation it performs on
Windows, as of 2026-10-18 (main `39c8f56` + Sandro's branches `sandro/displays`,
`sandro/website-design`, `sandro/launcher`). Built from a full read of the code; file references are
relative to the repo root.

**Status legend**

| Mark | Meaning |
|---|---|
| ✅ | Works in the real apps (`kvm_host.exe` / `kvm_client.exe`, started directly or by `sarke.exe`) |
| 🚀 | Works when Sarke is started with **`sarke.exe`** (the launcher, §2.0) |
| 🧪 | Built and tested, but only reachable from a tool exe |
| ⚠️ | Works with a limitation (stated) |
| ⏳ | Planned or designed, not built |

---

## 1. What Sarke is

Two Windows 10/11 PCs on the same local network become one desk. **A** (host) runs
`kvm_host.exe`, **B** (client) runs `kvm_client.exe`. Everything is symmetric (protocol v2): either
PC's mouse and keyboard can control the other, and either PC can send a window or files to the
other. Apps never leave the PC they run on; the other PC shows their picture and sends input back.

**Pieces you ship:** `sarke.exe` (start this), `kvm_host.exe`, `kvm_client.exe`, `config.json`, and
`assets\fonts\` (the UI fonts; without them the UI falls back to Segoe UI). Static CRT: no
installer, no redistributables.
**Optional:** the Virtual Display Driver (VDD) on a PC that should send windows; firewall rules.

---

## 2. Feature catalogue

### 2.0 Starting Sarke — `sarke.exe` 🚀
Double-click `sarke.exe` on each PC. It runs `kvm_host.exe` (host) or `kvm_client.exe` (client) in
the background — no console window — and gives it a face: the tray icon, the notification cards,
the Display arrangement window and a settings menu (§3.3). It learns what the app does from its log
and stops it with Ctrl+Break, so windows come home and the virtual display is detached.
- **Role:** `--host` / `--client`, else the one remembered in `%LOCALAPPDATA%\Sarke\settings.json`, else on the first run: a host found on the network → client, none → host. Switch it in the menu.
- **Config:** the `config.json` the apps would find (created with all defaults if missing); passed to the app with `--config`, so the menu, Displays and the app always use the same file.
- **One per user session:** starting it again shows Display arrangement and "Sarke is already running".
- **Scripts:** `sarke.exe --quit`, `sarke.exe --restart`; `-- <args>` passes options to the app.
- **Log:** `%LOCALAPPDATA%\Sarke\logs\host.log` / `client.log` (this run; the previous one is `.old.log`).
- **If it dies,** the app is ended with it (job object). If the app exits on its own, a card says why.
- Code: `client_b/launcher.*`, `tools/sarke.cpp`; tested by `tools/launcher_test.exe` and end-to-end with a real host + client.

### 2.1 Connecting ✅
| What | How |
|---|---|
| Host listens | TCP 50500 control, 50501 video, 50502 files; UDP 50503 discovery (`host_a/main.cpp`) |
| Client finds the host by itself | `host_ip: "auto"` (default): broadcast on UDP 50503, three tries in 1.5 s, prefers the last host's name; several hosts → asks in the console. `--host <ip>` skips it (`common/src/discovery.cpp`, `client_b/main.cpp`) |
| Handshake | `hello` both ways: protocol version, primary screen size, DPI scale, hostname, desk flag. Different protocol version → rejected |
| Reconnect | Client retries every 1 s; host waits for the next client |
| Liveness | ping every 1 s, link dropped after 5 s of silence; RTT and clock offset logged every 5 s |
| ⚠️ One session | The host serves **one** client at a time (the website says "2 or more PCs") |

### 2.2 Mouse and keyboard across the edge (KVM) ✅ both ways
- Push the cursor off the screen edge facing the other PC (`config.json client_side`) with no button held → the other PC takes over the cursor at the same relative height; your keyboard follows. Raw Input gives unaccelerated motion (fallback: hook deltas).
- Push back off the facing edge on the other PC → control returns.
- Moving your own mouse on a controlled PC (24 px within 300 ms) takes it back.
- Buttons, both wheels, keys (extended keys, AltGr) are forwarded; anything held is released on handover or disconnect.
- Code: `common/src/input_capture.cpp`, `input_inject.cpp`, `kvm_link.cpp`, `win32_input.cpp`, `dpi.cpp` (Demetre).
- ⚠️ Sides: only `left` / `right` are accepted by the apps; `top` / `bottom` are implemented below them but rejected in both `main.cpp` (host turns KVM off, client exits).
- ⚠️ Elevated (Administrator) windows only take input if the app also runs as Administrator.

### 2.3 Emergency exit — Ctrl+Alt+Shift+Q ✅
Low-level keyboard hook, checked first, always: cursor home, `focus_leave` to the peer, **hooks removed until restart**, desk mode ended (windows come home, VDD detached).
⚠️ It does not stop the other PC from controlling this one; after it, moving your own mouse no longer takes the PC back.

### 2.4 Desk mode — the other PC's screen as one more monitor ✅ (default on)
- When both PCs have `desk: true` and the VDD is installed: a virtual monitor the size/scale of the peer's screen is attached on the peer's side; the cursor physically moves onto it.
- Drag any window onto that monitor: it keeps running here and is drawn on the peer's desktop at the same place (per-window capture, JPEG 4:4:4 with alpha for rounded corners). Clicks on it stay with the owner; clicks elsewhere go to the peer's desktop; the keyboard follows the last click.
- Ctrl+Alt+Shift+W moves the window in front onto the VDD (maximized) or back.
- Apps opened within 10 s of a click on the VDD open there.
- Link drop: desk goes off at once, VDD detached after 8 s; a quick reconnect resumes.
- Code: `host_a/window_link.cpp`, `desk_share.cpp`, `desk_tracker.cpp`, `desk_input.cpp`, `virtual_display.cpp`, `client_b/desk_view.cpp` (Temo).
- ⚠️ Without the VDD a PC can only show the peer's windows, not send its own.

### 2.5 Send a window to the viewer (non-desk) — Ctrl+Alt+Shift+W ✅
The window in front moves to the VDD (maximized), is captured (WGC, JPEG 4:2:0 at `jpeg_quality`, ≤ `max_fps`) and shown in the **viewer** on the other PC. Clicks, wheel and keys in the viewer drive the original window. It comes back on W again, closing the viewer, closing the window, or disconnect. Code: `host_a/window_share.cpp`, `window_mover.cpp`, `video_sender.cpp`, `remote_input.cpp`, `client_b/viewer_*`.

### 2.6 Viewer window ✅ (design v2 = the website)
States: Waiting · Connecting · Linked · Streaming · Stalled · Connection lost. HUD: RTT, FPS, latency, bitrate, resolution, quality, dropped. Input is sent only over the picture. See §3.1 for its buttons.
⚠️ In the apps the viewer's endpoint is never set, so Waiting/Connecting show no address; only Linked/Lost are driven.

### 2.7 Files ✅ both ways (TCP 50502)
| Way | What happens |
|---|---|
| Ctrl+Alt+Shift+F | A "Drop here to send" box appears at the cursor for 5 s; files dropped on it go to the other PC |
| Drag files over the edge | Holding a file drag at the edge: the files are sent, control moves over, and the drag continues on the other PC — release to drop them anywhere there |
| Desk mode | A file drag onto the peer's desktop on the VDD is handed over the same way |
| Received files | `%USERPROFILE%\Downloads\KVM\` (folders kept, name clashes → " (2)"), Explorer opens on them; drags in progress are staged in `%TEMP%\Sarke\drag\` |

### 2.8 Clipboard ✅ both ways
Copy on one PC, paste on the other: text (UTF-8), images (PNG / bitmap → PNG), and **files copied in Explorer** (announced on the clipboard channel, sent over the file channel, pasted with Ctrl+V). Limits: 16 MB text/images; 4 GB / 20000 files. `--no-clipboard` turns it off. Code: `common/src/clipboard.cpp` (Demetre).

### 2.9 Display arrangement ("Where is the other PC?") 🚀
Shows this PC's real monitors live, the other PC, and the exact edge where the cursor crosses; drag the other PC to a side (or Left/Right buttons, arrow keys); **Apply writes `client_side` into config.json** (correctly for host or client, keeping every other key). Identify flashes each monitor's number; "Windows display settings ›" opens Windows' page. Open it from the Sarke tray icon (click, or Display arrangement... in the menu) — **after Apply the launcher restarts the app with the new side**. Also standalone: `tools\sarke_displays.exe`. Code: `client_b/display_layout.*`, `display_settings.*`.
⚠️ Each PC keeps its own config (the status line says what to set on the other one); top/bottom locked (see 2.2).

### 2.10 Tray icon and notification card 🚀
Tray: the Sarke mark in "show hidden icons", tooltip "Sarke · Host · Linked to tabuladze · 3 ms", the menu of §3.3. Notifications: Sarke's own card at the **top-right** (like NVIDIA's overlay, in the website's style): "Sarke is running", "Connected to {PC}" (address · screen · scale), "Connection lost" (host: waiting for it to come back; client: reconnecting), "Sarke has a problem" (an error line, e.g. a bad config), "Sarke stopped", "Restarting Sarke"; hover holds, click opens Display arrangement, × closes; fades only when Windows animations are off. Each kind can be turned off in the menu. Code: `client_b/tray_icon.*`, `toast.*`. Preview: `tools\ui_preview_toast.exe --sequence`.

### 2.11 Look and feel ✅
The website's design system (docs/DESIGN.md v2): dark and light themes following Windows' app mode (`SARKE_THEME=light|dark` forces one), Google Sans + Geist Mono, rounded tiles, pill buttons, Windows 11 rounded windows, the Sarke mark as the icon everywhere.

---

## 3. Every control

### 3.1 Viewer window ✅
| Control | Does |
|---|---|
| Title strip drag / double-click | Move / maximize (Snap works) |
| Edges (6 dp) | Resize |
| **HUD** | Show / hide the HUD |
| **Fit / 1:1** | Fit the picture to the window, or pixel-exact centred (cropped) |
| Minimize / Maximize | As in Windows |
| Close (×) | Gives the window back to its owner (`stream_stop`); in the apps the viewer hides |
| Mouse / wheel / keys over the picture | Sent to the original window; letterbox ignored; Alt+F4 stays local |

### 3.2 Display arrangement window 🧪
| Control | Does |
|---|---|
| Drag the other PC | Shows drop zones, previews the side, snaps on release |
| Left / Right | Pick the side (Top / Bottom disabled until the apps accept them) |
| Host / Client | Which role this PC has (guessed from the running app, then `host_ip`) |
| **Apply** (Enter) | Writes `client_side` to config.json atomically |
| **Revert** (Esc) | Back to what config.json says |
| **Identify** | Big number on each monitor for 2.5 s |
| **Windows display settings ›** | Opens `ms-settings:display` |
| Arrow keys | Pick a side |
| Minimize / Close | As in Windows |

### 3.3 Tray icon and menu 🚀 · Notification card 🚀
Left click / double click = Display arrangement. Right click:
| Item | Does |
|---|---|
| Sarke · This PC is the host | (state, not clickable) |
| **Display arrangement…** | Opens the window (§3.2); Apply restarts the app |
| Window quality ▸ Low 50 · Medium 75 · High 90 · Best 95 | Writes `jpeg_quality`, restarts the app (a custom value shows as "Custom (n)") |
| Frame rate ▸ 15 · 30 · 60 fps · No limit | Writes `max_fps` (0 = no limit), restarts |
| Desk mode (the other PC as a monitor) ✓ | Writes `desk`, restarts |
| Notifications ▸ When the other PC connects ✓ · When the connection drops ✓ | Saved in `settings.json`, no restart |
| Open received files | `Downloads\KVM` in Explorer |
| Open config.json | In Notepad |
| Show log | This run's log in Notepad |
| This PC is the ▸ Host (main PC) · Client | Saves the role, restarts as the other app |
| Restart Sarke / Start Sarke | Restarts (or starts, if it stopped) the app |
| Quit Sarke | Stops the app gracefully, then the launcher |
Card: click = Display arrangement, × = close, hover = hold.

### 3.4 Drop box ✅
Ctrl+Alt+Shift+F shows it; dropping files sends them; it hides after 5 s (or 0.7 s after a drag leaves).

### 3.5 Hotkeys (global, both PCs)
| Keys | Does | Status |
|---|---|---|
| Ctrl+Alt+Shift+Q | Emergency exit (§2.3) | ✅ |
| Ctrl+Alt+Shift+W | Send / bring back the window in front | ✅ |
| Ctrl+Alt+Shift+F | Drop box for files | ✅ |
| Ctrl+Alt+Shift+S | Window picker overlay | ⏳ designed (DESIGN.md §3), not built — the website's docs list it |
| Ctrl+C in the console | Quit (VDD detached, windows brought home) | ✅ |
While one PC controls the other, these keys are forwarded so they fire on the controlled PC.

---

## 4. Command line

**kvm_host.exe** — `--config <path>` · `--log <file>` · `--no-kvm` (no hooks, no Ctrl+Alt+Shift+Q, no edge drags) · `--no-clipboard` · `--no-desk` · `--test-video [--test-video-kb N] [--test-video-fps N]` (synthetic 30 fps stream for network tests).

**kvm_client.exe** — `--host <ip>` (skip discovery) · `--config <path>` · `--log <file>` · `--no-kvm` · `--no-clipboard` · `--no-desk` · `--desk-no-vdd` (show the host's windows but never attach this PC's VDD; one-PC tests).

**sarke.exe** — `[--host | --client] [--config <path>] [-- <args for the app>]` · `--quit` · `--restart`.

**Tools** — `sarke_displays.exe [--role host|client] [--config <path>] [--peer 1920x1200@1.5 <name>]`; `ui_preview_viewer.exe --state waiting|connecting|linked|streaming|stalled|lost [--hidden]`; `ui_preview_toast.exe [--kind connected|lost|info] [--sequence] [--motion on|off]`; `ui_preview_logo.exe [--ico <file>]`.

---

## 5. config.json

Found in the working directory, else next to the exe and up to three parent folders. Missing file or keys → defaults; malformed JSON → the app exits.

| Key | Default | Effect |
|---|---|---|
| `host_ip` | `"auto"` | Client: the host's address, or `"auto"` to discover it. Host: unused |
| `control_port` | 50500 | Control + input (discovery tells the client the host's value) |
| `video_port` | 50501 | Pictures — must match on both PCs |
| `file_port` | 50502 | Files — must match on both PCs |
| `client_side` | `"right"` | Where B stands next to A (`left` / `right`); the KVM edge and the VDD side. Written by the Displays window |
| `jpeg_quality` | 75 | Window stream quality (desk mode uses at least 80) |
| `max_fps` | 30 | Frames per second sent (0 = no cap) |
| `desk` | `true` | Desk mode on/off (also `--no-desk`) |

Sarke's own files in `%LOCALAPPDATA%\Sarke\`: `settings.json` (launcher: role, notifications), `peer.json` (the last PC seen — written by the launcher, read by Display arrangement), `logs\host.log` / `client.log` (+ `.old.log`).

---

## 6. What it does on Windows (system operations)

| Area | Operations | Where |
|---|---|---|
| **Input injection** | `SendInput` (mouse moves, buttons, wheels, keys — tagged so the hooks ignore them), `SetCursorPos` | `win32_input.cpp`, `input_inject.cpp`, `input_capture.cpp`, file drag/drop |
| **Input capture** | `WH_MOUSE_LL` + `WH_KEYBOARD_LL` hooks, Raw Input (mouse, background), `RegisterHotKey` (W, F), WinEvent hooks (window tracking) | `input_capture.cpp`, `hotkeys.cpp`, `desk_tracker.cpp` |
| **Displays** | Attach / move / resize / detach the Virtual Display Driver monitor (`ChangeDisplaySettingsEx`, persisted in the registry by Windows; fallback `SetDisplayConfig` extend), set its scale (`DisplayConfigSetDeviceInfo`) | `virtual_display.cpp` |
| **Other apps' windows** | Restore / move / maximize (`SetWindowPos`, `SetWindowPlacement`), bring to the foreground (`AttachThreadInput` + `SetForegroundWindow`), cancel menus, move newly opened apps onto the VDD; elevated windows are refused | `window_mover.cpp`, `remote_input.cpp`, `desk_tracker.cpp` |
| **Screen capture** | Windows.Graphics.Capture of monitors and single windows (D3D11, WARP fallback), borderless | `capture.cpp` |
| **Clipboard** | Read/write text, PNG/DIB, file lists (`CF_HDROP` + Preferred DropEffect) | `clipboard.cpp` |
| **Drag and drop** | OLE drop target (drop box), `DoDragDrop` to continue a drag on the other PC | `file_drop.cpp`, `file_drag.cpp` |
| **Files written** | `Downloads\KVM\…` (received), `%TEMP%\Sarke\drag\…` (⚠️ never cleaned), `--log` file; Displays and the launcher menu: `config.json` keys (atomic, other keys kept); launcher: `%LOCALAPPDATA%\Sarke\settings.json`, `peer.json`, `logs\` | `file_transfer.cpp`, `display_layout.cpp`, `launcher.cpp` |
| **Network** | Host listens on all interfaces: TCP 50500–50502, UDP 50503; client connects + broadcasts; `TCP_NODELAY`. **No encryption, no pairing** | `net.cpp`, `discovery.cpp` |
| **Firewall** | Not touched by the apps. Scripts add inbound rules for TCP 50500–50502 and UDP 50503 (admin): `tools\setup_env.ps1 -Firewall`, `tools\test_package\1_allow_firewall.cmd` | scripts |
| **Processes started** | `explorer.exe /select` (received files); `ms-settings:display` (Displays); the launcher starts `kvm_host.exe` / `kvm_client.exe` (hidden console, own process group, in a kill-on-close job), sends it Ctrl+Break to stop, ends it after 9 s if it hangs; opens Explorer / Notepad from the menu | `file_link.cpp`, `display_settings.cpp`, `launcher.cpp` |
| **Registry** | Only indirectly (display changes); reads the app-mode theme value | — |

Security note: anyone on the same network who can reach the ports can connect and inject input. Use it on trusted networks only (the website says the same).

---

## 7. Network protocol (v2)

Control `[u32 len][JSON]` on 50500 · video `[u32 hdr][u32 payload][header][payload]` on 50501 · files, same framing, on 50502 · discovery JSON on UDP 50503. Messages: `hello`, `focus_enter`, `focus_leave`, `mouse_move`, `mouse_button`, `mouse_wheel`, `key`, `desk_move`, `desk_cursor`, `desk_layout`, `stream_start`, `stream_stop`, `remote_mouse_move`, `clipboard` (text / png / files), `ping` / `pong`; video header `frame`; files `transfer_begin`, `file_begin`, `file_chunk`, `file_end`, `transfer_end`; discovery `kvm_discover`, `kvm_here`. Full table: `CLAUDE.md`, `common/include/kvm/protocol.h`.

---

## 8. Tools and tests

| Safe to run unattended | `protocol_test`, `queue_test`, `dpi_test`, `display_layout_test` (62 checks), `launcher_test` (25), `jpeg_test`, `video_sender_test`, `file_transfer_test`, `video_test`, `ui_preview_logo`; `clipboard_test` (overwrites your clipboard); `docs/preview/test_sarke_app.py` (99 browser checks) |
|---|---|
| **Move the mouse / install hooks / attach the VDD** — don't touch the PC while they run | `win32_input_test`, `input_capture_test`, `input_inject_test`, `kvm_link_test`, `desk_hook_test`, `kvm_local_test` (interactive), `file_drag_test`, `vdd_test`, `window_mover_test`, `capture_test`, `window_share_test`, `remote_input_test`, `desk_test` |
| **UI to look at** | `sarke` (the launcher), `ui_preview_viewer`, `ui_preview_toast`, `sarke_displays`, `viewer_test`, `fake_streamer`, `kvm_fake_peer` |
| **Two PCs** | `kvm_host` + `kvm_client`, `tools\test_package\1–5` (firewall, run, diagnose, test video) |

---

## 9. Gaps, and where the website and the app disagree

| # | Gap | Who |
|---|---|---|
| 1 | Tray, cards, Display arrangement and `peer.json` work through `sarke.exe`; running `kvm_host` / `kvm_client` directly still gives only a console. The viewer's HUD on/off isn't a saved setting (it is per window) | — (Luka may still build them into `main.cpp`) |
| 2 | Top / bottom placement rejected by both `main.cpp` | Luka |
| 3 | Website download: release repo is `TODO`, so "Download coming soon"; no release zip published yet | Website + whoever publishes releases |
| 4 | Website says `sarke_host.exe` / `sarke_client.exe`; the exes are still `kvm_host.exe` / `kvm_client.exe` | Luka (exe names) or the website |
| 5 | Website: "2 or more PCs"; the host serves one client at a time | product decision |
| 6 | Website ports "50500–50501"; the app also needs TCP 50502 (files) and UDP 50503 (discovery) | website |
| 7 | Website docs: `sarke_client.exe --host <A's IP>` is required; since discovery it is optional (`host_ip: "auto"`) | website |
| 8 | Website docs list Ctrl+Alt+Shift+S (window picker); not built | Temo / website |
| 9 | Website doesn't mention clipboard sync, desk mode's "apps open where you work", or Ctrl+Alt+Shift+F | website |
| 10 | Viewer endpoint never set by the apps (Waiting / Connecting show no address) | Luka / Temo |
| 11 | HUD "Quality" shows the receiver's own `jpeg_quality`, not the sender's | protocol (Luka) |
| 12 | `%TEMP%\Sarke\drag\` folders are never cleaned | Temo |
| 13 | Ctrl+Alt+Shift+Q doesn't stop being controlled; host and client on one PC conflict on the W/F hotkeys | Demetre / note |
| 14 | No encryption or pairing; LAN only | planned |
| 15 | No exe icon resource / FileDescription (`docs/brand/sarke.ico` is ready) | Luka |
| 16 | Not verified yet: the v2 design at 125 / 150 / 200 % scale, and the full two-PC run with the new UI and `sarke.exe` (one-PC end-to-end run done: connect / lost cards, Apply → restart, `--restart`, `--quit`) | Sandro |
| 17 | The packaging script and the website should ship and tell people to start `sarke.exe` | Luka (zip), website |
| 18 | The tray menu is Windows' own menu (light even in dark mode) | — |
