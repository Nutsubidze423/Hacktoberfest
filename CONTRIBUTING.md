# Contributing to Sarke

Thanks for helping. Sarke is a C++20 Windows app; this page is how we work on it. The plan, with the
decisions already made, is [docs/ROADMAP.md](docs/ROADMAP.md): read it first, and pick up the next
unticked task of the current phase, or open an issue to discuss something else before you start.

Security problems: please don't open an issue. Report them privately (see
[docs/SECURITY.md](docs/SECURITY.md)).

## How we work

- **One task per branch:** `phaseN/<task>` (e.g. `phase1/tls`). Pull requests go to `main`; nobody
  commits to `main` directly.
- **Phase 1 (security) is merged before any Phase 2+ work lands.**
- **A task is done when, on its branch:**
  1. it builds without warnings (`/W4 /WX`);
  2. `ctest --preset ci` passes, and `ctest --preset local` where the change can affect it;
  3. CI is green;
  4. the [regression checklist](docs/ROADMAP.md#regression-checklist) rows it touches are run. The PR
     says which two-PC rows still need a person.
  5. it is ticked in `docs/ROADMAP.md`;
  6. the docs are updated.
- **Small, reviewable changes:** each PR explains how to run and test it.
- **New dependencies:**
  - say why one is needed and what its license is;
  - add it to [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) in the same PR.
- **Measure before you claim.** No performance, compatibility or "it works" without having measured
  or run it. Numbers go in `docs/PERF.md`.
- **If the plan turns out wrong,** update the roadmap first, then the code.
- **Third-party code:** Sarke is Apache-2.0. Don't copy GPL code (Sunshine and Moonlight are
  GPL-3.0); reading how they behave is fine.

## Build and test

One-time setup: `powershell -ExecutionPolicy Bypass -File tools\setup_env.ps1` (Visual Studio 2022
C++ tools, CMake, Ninja, vcpkg). Then, in a *Developer PowerShell for VS 2022*:

```powershell
cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset ci       # unit + loopback (the real kvm_host + kvm_client on 127.0.0.1), what CI runs
ctest --preset local    # + desktop, vdd and gpu tests: hands off the mouse for about three minutes
```

**Test labels** ([tools/CMakeLists.txt](tools/CMakeLists.txt)): every `tools/*_test.cpp` must be in
exactly one group, or configure fails.

| Label | Needs |
|---|---|
| `unit` | nothing; headless |
| `loopback` | the real programs on 127.0.0.1, on ports Windows picks. [tools/loopback_test.cpp](tools/loopback_test.cpp) is where two-PC paths get tested on one PC |
| `desktop` | an unlocked session: hooks, `SendInput`, the clipboard, OLE drag and drop |
| `vdd` | the Virtual Display Driver installed; a fixture opens Paint |
| `gpu` | a GPU with a D3D11 video processor / hardware codecs; CI runners have none, so only `local` runs it |
| `bench` | nothing; it prints timings |

`SARKE_STRICT_TIMING=1` (CI sets it) turns timing floors into hard checks. On a busy dev PC they
only warn.

**Fuzzing** ([tools/fuzz/](tools/fuzz/)): libFuzzer with AddressSanitizer, in its own preset so the
normal build stays uninstrumented. From a Developer PowerShell (it puts the ASan runtime on PATH):

```powershell
cmake --preset x64-fuzz
cmake --build --preset x64-fuzz
build\x64-fuzz\tools\fuzz\fuzz_protocol.exe -max_total_time=600 tools\fuzz\corpus
build\x64-fuzz\tools\fuzz\fuzz_annexb.exe -max_total_time=600 tools\fuzz\corpus_annexb
```

- `fuzz_protocol`: control messages and the video frame header (JSON).
- `fuzz_annexb`: the framing check on an H.264 payload ([kvm/annexb.h](common/include/kvm/annexb.h)).

`tools\fuzz\corpus` and `corpus_annexb` hold the seeds, and libFuzzer adds what it finds to the folder it is given: give
it a copy when you don't want new files there. A `crash-*` file is a bug to fix; add the input to the
seeds once it is fixed.

**Two PCs on one machine:** the two programs only talk to a paired PC. Give the client its own
identity folder, so it is a different "PC", and pair them once:

```powershell
cd build\x64-release
host_a\kvm_host.exe --print-id                                   # the host's device ID
client_b\kvm_client.exe --data-dir $env:TEMP\sarke-b --print-id  # the client's
host_a\kvm_host.exe --pair-with <client id>
client_b\kvm_client.exe --data-dir $env:TEMP\sarke-b --pair-with <host id> --host 127.0.0.1 --desk-no-vdd
```

`--pair-with` is remembered (`paired.json`), so later starts don't need it. Both logs show
`connected`, then `link: rtt …` every 5 s. If an installed Sarke is running, it holds ports
50500–50502: give the host other ports in a `--config` file. Two fakes stand in for the other side,
and should be extended rather than a test skipped:
- [tools/kvm_fake_peer.cpp](tools/kvm_fake_peer.cpp): the control channel (`--plaintext-v2` plays Sarke 1.1);
- [tools/fake_streamer.cpp](tools/fake_streamer.cpp): JPEG frames on the video port.

## Code style

- C++20, MSVC, warnings are errors.
- Formatting: clang-format 19.1.5 (the version Visual Studio 2022 ships) with
  [.clang-format](.clang-format). CI rejects unformatted code, so run `clang-format -i` on the
  files you touched, or format on save. Formatting-only commits go into `.git-blame-ignore-revs`.
- Sources are globbed: a new `.cpp` in `common/src`, `host_a`, `client_b` or `tools` needs no CMake
  change.
- Every source file starts with `// SPDX-License-Identifier: Apache-2.0`.
- **RAII** for every Win32 and COM resource; no raw `new`/`delete`.
- **Windows include order matters:** `winsock2.h` before `windows.h`, `shellapi.h` after it.
  Includes are never sorted automatically.

## Rules the code depends on

**Input and hooks**
- **Ctrl+Alt+Shift+Q always returns control to the local PC and removes the hooks.** Never remove
  or block it.
- Low-level hook callbacks only queue work: no logging, no network, no heavy allocation. Windows
  silently removes slow hooks.
- Injected input carries the KVM marker (`kInjectedMarker`), so our own hooks skip it.

**Screens and windows**
- Both processes are per-monitor DPI aware v2; all coordinates are physical pixels.
- Coordinates on a streamed window are normalized (0..1). The owner maps them onto the window's
  current client rect.

**Video**
- Always send the newest frame: drop old ones, never queue them (`LatestSlot`).
- H.264 changes where that happens, because a picture refers to the one before it:
  - the viewer decodes every frame it receives, in order, and drops pictures instead;
  - a sender that has to drop starts again at a keyframe (roadmap P2.6).

**Protocol**
- Every message is in [docs/PROTOCOL.md](docs/PROTOCOL.md); change it together with
  [protocol.h](common/include/kvm/protocol.h).
- Bump `kProtocolVersion` when an older peer can't follow.

**Security**
- No hand-rolled crypto.
- Once Phase 1 lands, nothing from an unpaired peer is parsed.
- Received files are written only through `safe_destination`.

## Map

| Path | What |
|---|---|
| `common/` | static lib `kvm_common`: protocol, networking, TLS, device identity and paired PCs (`kvm/tls.h`, `kvm/identity.h`, `kvm/paired.h`), video channel, discovery, files, clipboard, input hooks and injection, DPI, config, logging, queues |
| `host_a/` | `kvm_host.exe` (listens): window sharing, capture, JPEG, the virtual display, desk mode on the owner's side |
| `client_b/` | `kvm_client.exe` (connects) and the `sarke.exe` launcher UI: viewer, desk overlay, tray, display arrangement |
| `tools/` | `sarke.exe` entry point, tests, fakes, UI previews, `sarke_driver.exe`, scripts |
| `installer/` | Inno Setup script; `tools\fetch_vdd.ps1` fetches the driver into `installer\vdd` |
| `docs/` | [ROADMAP](docs/ROADMAP.md), [PROTOCOL](docs/PROTOCOL.md), [SECURITY](docs/SECURITY.md), [DESIGN](docs/DESIGN.md) (UI design system), [RELEASING](docs/RELEASING.md); `archive/hackathon/` is history |

## Releasing

[docs/RELEASING.md](docs/RELEASING.md): bump the version, build the installer, test it, tag, publish
the GitHub Release, then upload it to the website.
