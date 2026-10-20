# CLAUDE.md: working on Sarke

Sarke joins Windows PCs on one network into one workspace: the mouse and keyboard cross screen
edges (KVM, both ways), windows move to the other PC's screen while they keep running where they
started (desk mode and the window viewer), and the clipboard and file drags work across. The goal
(see [docs/ROADMAP.md](docs/ROADMAP.md)): *all your devices become one computer you can sit at from
anywhere*.

Talk to the user in English. Code, comments, commit messages and docs are in English.

## Start of every session

1. Read [docs/ROADMAP.md](docs/ROADMAP.md). It holds the decisions already made (don't reopen
   them), the phases, the next unticked task and the working rules below.
2. Check what is really done: `git status`, `git log --oneline -15`, the current branch. Don't trust
   a checkbox blindly.
3. Work on **one roadmap task at a time**.

## Working rules

**One task at a time**
- Every task gets its own branch, named `phaseN/<task>` (e.g. `phase1/tls`).
- **Never commit to `main`.**
- **Phase 1 (security) is merged before any Phase 2+ work lands.**

**Before a task counts as done**, all of these happen on the same branch:
1. The build passes.
2. `ctest --preset unit` passes, and `ctest --preset local` where the change can affect it.
3. CI is green.
4. The [regression checklist](docs/ROADMAP.md#regression-checklist) is run; tell the user exactly
   which manual two-PC rows to run.
5. The task is ticked in `docs/ROADMAP.md`.
6. The docs are updated.

**Change style**
- Keep changes small and reviewable.
- Explain how to run and test each one.

**Dependencies**
- No new dependency without saying why and noting its license.
- Add it to [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) in the same change.

**Honesty**
- If the plan proves wrong, stop and update the roadmap before continuing.
- Never claim performance, compatibility or "it works" without having measured or run it.
  Measured numbers go in `docs/PERF.md` (created by roadmap task P2.1).

**Third-party code**
- Don't copy GPL code (Sunshine, Moonlight); Sarke is Apache-2.0.
- Behaviour may be read as a reference.

## Build and test

One-time setup: `powershell -ExecutionPolicy Bypass -File tools\setup_env.ps1` (VS 2022 C++
tools, CMake, Ninja, vcpkg). In a "Developer PowerShell for VS 2022":

```powershell
cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset unit     # headless tests, also in CI
ctest --preset local    # + desktop (hooks, SendInput, clipboard) and vdd tests: hands off the mouse
```

**Test labels** ([tools/CMakeLists.txt](tools/CMakeLists.txt)):
- `unit`: headless.
- `desktop`: needs an unlocked session.
- `vdd`: needs the Virtual Display Driver; Paint is opened by a fixture.
- `bench`: timings only.

Every `tools/*_test.cpp` must be in one group, or configure fails. `SARKE_STRICT_TIMING=1` (set in
CI) turns timing floors into hard checks.

**Two PCs on one machine.** Only one PC is usually available. Use loopback:

```powershell
build\x64-release\host_a\kvm_host.exe
build\x64-release\client_b\kvm_client.exe --host 127.0.0.1 --desk-no-vdd
```

Both logs show `connected`, then `link: rtt ...` every 5 s. The fakes stand in for the other side:
- `tools/kvm_fake_peer.cpp`: plays the other PC on the control channel.
- `tools/fake_streamer.cpp`: sends JPEG frames on the video port.

Extend them rather than skipping a test.

**Code style**
- C++20, MSVC, `/W4 /WX`: warnings are errors.
- clang-format 19.1.5 (the version VS 2022 ships) with [.clang-format](.clang-format). CI rejects
  unformatted code: run `clang-format -i` on the files you touched.
- Sources are globbed: a new `.cpp` in `common/src`, `host_a`, `client_b` or `tools` needs no CMake
  change.

## Map

| Path | What |
|---|---|
| `common/` | static lib `kvm_common`: protocol, net, video channel, discovery, files, clipboard, KVM hooks/injection, DPI, config, log, queues |
| `host_a/` | `kvm_host.exe` (listens) + window share, WGC capture, JPEG encode, VDD control, desk mode owner side |
| `client_b/` | `kvm_client.exe` (connects) + viewer, desk view, the launcher UI (`sarke.exe`: tray, Displays) |
| `tools/` | `sarke.exe` entry point, tests, fakes, UI previews, `sarke_driver.exe`, scripts |
| `installer/` | Inno Setup script; `vdd/` gets the driver from `tools\fetch_vdd.ps1` |
| `docs/` | [ROADMAP](docs/ROADMAP.md), [PROTOCOL](docs/PROTOCOL.md), [DESIGN](docs/DESIGN.md) (UI design system), [RELEASING](docs/RELEASING.md); `archive/hackathon/` is history |

Ports: 50500 control and input, 50501 video, 50502 files (TCP), 50503 discovery (UDP). Every
message is in [docs/PROTOCOL.md](docs/PROTOCOL.md). Change it together with
[protocol.h](common/include/kvm/protocol.h), and bump `kProtocolVersion` when old peers can't
follow.

## Rules the code depends on

**Input and hooks**
- **Ctrl+Alt+Shift+Q always returns control to the local PC and removes the hooks.** Never remove
  or block it.
- Hook callbacks (`WH_MOUSE_LL`, `WH_KEYBOARD_LL`) only queue work: no logging, no network, no
  allocation-heavy code. Windows silently removes slow hooks.
- Injected input carries the KVM marker, so our own hooks ignore it.
- Elevated windows can't receive input from a normal process (UIPI).

**Screens and windows**
- Both processes are per-monitor DPI aware v2, and all math is in physical pixels.
- Coordinates on a streamed window are normalized (0..1). The owner maps them onto the window's
  *current* client rect.

**Video**
- Always the newest frame: drop old frames, never queue (`LatestSlot`).

**Code**
- Windows include order matters (`winsock2.h` before `windows.h`; `shellapi.h` after it). Includes
  are never sorted automatically.
- RAII for every Win32/COM resource; no raw `new`/`delete`.

## Security

**Today**
- There is no authentication and no encryption: any device on the local subnet can connect.
- Never widen that exposure: no internet listeners, no relaxed firewall rules.
- Phase 1 of the roadmap fixes this with pairing, device keys and TLS 1.3.

**Always**
- No hand-rolled crypto.
- Once Phase 1 lands, nothing from an unpaired peer is parsed.
- Received files are written only through `safe_destination`.

## Releasing

[docs/RELEASING.md](docs/RELEASING.md): bump `resources/version.h`, `tools\make_installer.ps1`, test,
tag, GitHub Release, then the website (`tools\publish_release.ps1`).
