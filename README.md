# Sarke

[![CI](https://github.com/Nutsubidze423/Hacktoberfest/actions/workflows/ci.yml/badge.svg)](https://github.com/Nutsubidze423/Hacktoberfest/actions/workflows/ci.yml)

One desk, two Windows PCs.

Sarke joins two Windows PCs on the same network into one workspace. Move the mouse past the edge
of the screen and it controls the other PC. Drag a window across and it appears on the other screen
while it keeps running where it started. Drag files across, copy on one PC and paste on the other.

Tools like Mouse Without Borders share a mouse and keyboard; tools like spacedesk turn a second
device into a monitor. Sarke does both at once: the other PC's screen works as an extra monitor,
and that PC stays fully usable with its own apps, mouse and keyboard.

## Features

- **Shared mouse and keyboard.** Cross the screen edge to control the other PC. Works in both
  directions; the other PC can sit left, right, above or below.
- **Desk mode.** The other PC's screen becomes one more monitor. Windows dragged onto it keep
  running on their own PC and are drawn on the other screen, pixel for pixel, on top of its desktop.
  Both PCs' mice and keyboards can use them.
- **Files.** Drag files across the edge and they land where you let go: a folder, the desktop or
  an app. Files copied with Ctrl+C can be pasted on the other PC.
- **Clipboard.** Text and images copied on one PC paste on the other.
- **Finds the other PC.** No IP addresses to type. Sarke lists the PCs running it on the network;
  pick one, and optionally let Sarke connect to it automatically next time.
- **Arranged once.** Set where the other PC stands on either of the two; both follow at once, and
  each PC remembers the place of every PC it has been connected to.
- **Window viewer.** Without a virtual display, any window can still be streamed into a viewer on
  the other PC and controlled from there.

## Requirements

- Two PCs with Windows 10 (version 2004 or later) or Windows 11, 64-bit.
- The same local network. Sarke uses TCP ports 50500–50502 and UDP port 50503.
- For desk mode, the [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)
  on the PC whose windows go to the other screen. The installer sets it up.

## Install

1. Download `SarkeSetup-<version>.exe` from the
   [latest release](https://github.com/Nutsubidze423/Hacktoberfest/releases/latest) (or the
   [website](https://sarke-nine.vercel.app)) and run it on both PCs. It installs Sarke, the virtual display driver and the firewall rules for
   the local network.
2. Start Sarke on both PCs. It runs in the system tray and shows the PCs running Sarke on the
   network.
3. On one PC, click the other PC in that list to connect. Turn on **Auto** to connect to it by
   itself whenever it is on the network. The list is also under **Connect to another PC** in the
   tray menu.
4. Choose **Display arrangement** in the tray menu to set which side of this screen the other PC is
   on. The change applies on both PCs at once.

Then move the mouse across that edge.

## Shortcuts

| Keys | Action |
|---|---|
| Ctrl+Alt+Shift+Q | Emergency stop: control returns to this PC and input capture stops |
| Ctrl+Alt+Shift+W | Send the window in front to the other PC, or bring it back |
| Ctrl+Alt+Shift+F | Show a drop box: files dropped on it go to the other PC |

## Configuration

Settings live in `config.json`. An installed Sarke keeps it in `%LOCALAPPDATA%\Sarke\`; a build
from source uses the one in the repository. Most of them can be changed from the tray menu.

| Key | Default | Meaning |
|---|---|---|
| `host_ip` | `"auto"` | The PC to connect to; `"auto"` finds it on the network |
| `control_port` | `50500` | Control and input |
| `video_port` | `50501` | Window pictures |
| `file_port` | `50502` | File transfers |
| `client_side` | `"right"` | For a PC not in `peers` yet: where the client stands next to the host (`left`, `right`, `top` or `bottom`) |
| `peers` | `{}` | Where each PC this one has been connected to stands, by computer name; set by Display arrangement |
| `jpeg_quality` | `75` | Picture quality, 1–100 |
| `max_fps` | `30` | Frame rate limit, `0` for none |
| `desk` | `true` | Desk mode; `false` uses the window viewer |

Logs are in `%LOCALAPPDATA%\Sarke\logs\`.

## Building from source

You need Visual Studio 2022 with *Desktop development with C++*, CMake 3.25 or later, Ninja and
[vcpkg](https://vcpkg.io) (`VCPKG_ROOT` set). `tools\setup_env.ps1` installs all of them.

From a *Developer PowerShell for VS 2022*:

```powershell
cmake --preset x64-release
cmake --build --preset x64-release
build\x64-release\tools\sarke.exe
```

The executables are statically linked: copying `sarke.exe`, `kvm_host.exe`, `kvm_client.exe`,
`config.json` and `assets\fonts` to another PC is enough to run them there.

To build the installer (needs [Inno Setup 6](https://jrsoftware.org/isinfo.php); the script fetches
the Virtual Display Driver with `tools\fetch_vdd.ps1`, pinned and hash-checked, since its binaries
aren't kept in git). Releasing it is described in [docs/RELEASING.md](docs/RELEASING.md):

```powershell
powershell -ExecutionPolicy Bypass -File tools\make_installer.ps1
```

The build treats warnings as errors. C++ is formatted with clang-format 19.1.5 (the version Visual
Studio 2022 ships) using [.clang-format](.clang-format); CI fails on unformatted code, so run
`clang-format -i` on the files you changed, or let your editor format on save.

Each `tools\*_test.cpp` builds into its own test executable, registered with ctest under a label
that says what it needs:

```powershell
ctest --preset unit    # headless tests, also run by CI
ctest --preset local   # + desktop (hooks, SendInput, clipboard) and vdd (Virtual Display Driver) tests
```

`local` moves the mouse, attaches the virtual display and opens a Paint window for about three
minutes, so keep your hands off the mouse and keyboard while it runs. To try both sides on one PC,
start `kvm_host.exe`, then `kvm_client.exe --host 127.0.0.1 --desk-no-vdd`.

## How it works

Each PC runs Sarke's tray app, `sarke.exe`, which starts one of two background processes: the
host (`kvm_host.exe`), which waits for a connection, or the client (`kvm_client.exe`), which
connects to a host. Once connected, both sides can do everything; the roles only decide who
listens.

| Port | Protocol | Carries |
|---|---|---|
| 50500 | TCP, length-prefixed JSON | Handshake, arrangement, input, clipboard, window layout |
| 50501 | TCP, JSON header + payload | Window pictures (JPEG) |
| 50502 | TCP, JSON header + payload | Files |
| 50503 | UDP broadcast | Finding the other PCs |

- **Input.** Low-level keyboard and mouse hooks with Raw Input capture input on the PC that has it.
  The other PC replays it with `SendInput`.
- **Desk mode.** A virtual monitor the size of the other PC's screen is attached on its side. Each
  window on that monitor is captured with Windows.Graphics.Capture and encoded as JPEG with an alpha
  mask for rounded corners. The other PC draws it in a transparent, click-through overlay at the
  same coordinates. A click lands on a shared window if one is under the cursor, otherwise on the
  other PC's own desktop.
- **Files.** A drag that reaches the other screen is caught, the files are sent, and the drag goes
  on from the cursor over there until the button is released.

Every message type is listed in [docs/PROTOCOL.md](docs/PROTOCOL.md). The hackathon's catalogue of
features is kept in [docs/archive/hackathon/BLUEPRINT.md](docs/archive/hackathon/BLUEPRINT.md).

## Project layout

```
common/      shared library: protocol, networking, input capture and replay, clipboard, files
host_a/      kvm_host.exe: window capture, virtual display, desk mode on the sending side
client_b/    kvm_client.exe and the tray app: viewer, desk overlay, launcher, UI
tools/       sarke.exe entry point, tests, previews and scripts
installer/   Inno Setup script; the virtual display driver is fetched by tools\fetch_vdd.ps1
docs/        roadmap, protocol, design system, releasing; archive/hackathon is history
```

## Limitations

- Windows only.
- Local network only, and traffic is not encrypted yet: use Sarke on networks you trust.
- One connection at a time.
- Window pictures are JPEG, so fast video is less smooth than locally; sound stays on the PC
  that plays it.
- Windows running as administrator can only be controlled when Sarke runs as administrator too.
- Builds are not code-signed, so Microsoft Defender may flag the input hooks. The installer can add
  an exclusion.

## License

Sarke is open source under the [Apache License 2.0](LICENSE). The third-party components it
includes keep their own licenses; their full texts are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Credits

Built by Luka, Demetre, Temo and Sandro (see [AUTHORS](AUTHORS)).

Sarke includes or uses:

- [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) (MIT)
- [libjpeg-turbo](https://libjpeg-turbo.org) (BSD-3-Clause, IJG, zlib), with
  [libspng](https://libspng.org) (BSD-2-Clause) and [zlib](https://zlib.net) (Zlib)
- [nlohmann/json](https://github.com/nlohmann/json) (MIT)
- Google Sans and Geist Mono fonts (SIL Open Font License 1.1)
