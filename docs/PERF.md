# Sarke performance measurements

Every number Sarke's docs, README or PRs claim comes from this page, together with how it was
measured and on what hardware. Older numbers stay as history.

## Machines

| Name | CPU | GPU | Windows |
|---|---|---|---|
| **Celeron laptop** (Demetre's dev PC) | Intel Celeron N4020, 2 cores, 1.1 GHz | Intel UHD Graphics 600 | 10 Pro 22H2 (19045) |

## TLS 1.3 overhead (roadmap P1.2, P1.5)

**What:** the round trip of a 64-byte message over 127.0.0.1, echoed by the other end. The same
socket pair runs once as plain TCP (`TCP_NODELAY`) and once through `kvm::tls::Stream` (OpenSSL
3.6.5, TLS 1.3). Median of 2000 round trips. Measured in `tools/tls_test.cpp` (it prints, it
doesn't check).

| Date | Machine | Plain TCP | TLS 1.3 | Added by TLS |
|---|---|---|---|---|
| 2026-10-20 | Celeron laptop, 3 runs | 56 / 59 / 58 µs | 93 / 96 / 94 µs | about +37 µs |

**What it means:** a mouse move or key press crosses the control channel once, so TLS adds about
0.04 ms on this CPU. A home Wi-Fi round trip is a few milliseconds, and the phone hotspot at the
hackathon measured 11–591 ms. This is a loopback figure: it shows the CPU cost of TLS, not the
network.

## Video channel over TLS 1.3 (roadmap P1.7)

**What:** the same tests built twice: from `main` before P1.7 (plain TCP) and from
`phase1/side-channels` (TLS 1.3, OpenSSL 3.6.5). Both run on 127.0.0.1, so one CPU does both
the encrypting and the decrypting. Runs alternated, with nothing else building.
- **Throughput:** `video_test`, 300 frames of 250 KB as fast as the channel takes them.
- **Frame time:** `video_sender_test` step 4. A synthetic 1920×1080 picture is JPEG-encoded
  (quality 75) and sent at most 30 fps. Measured from capture until B has the frame.

| Date | Machine | | Plain TCP | TLS 1.3 |
|---|---|---|---|---|
| 2026-10-21 | Celeron laptop, 3 runs | throughput | 4677 / 5243 / 4543 frames/s (1136–1311 MB/s) | 914 / 851 / 912 frames/s (213–228 MB/s) |
| 2026-10-21 | Celeron laptop, 3 runs | capture → received, 30 fps cap | 30.9 / 31.7 / 30.8 ms, 30 fps | 26.9 / 27.5 / 31.8 ms, 30 fps |

**What it means:**
- TLS costs about 1 ms of CPU per 250 KB frame here, for encrypt and decrypt together.
- 30 fps of 250 KB frames is 7.5 MB/s, about 3% of what the TLS channel carried.
- The frame time didn't change measurably: the run-to-run spread (27–32 ms) is bigger than the
  TLS cost, and both builds kept 30 fps.
- Over a real network each PC does only half of the TLS work, and Wi-Fi bandwidth is the limit
  long before this is.

## Video: capture to screen (roadmap P2.1 baseline)

**How it's measured:**
- `kvm_client --stats run.csv` (or `kvm_host`, whichever PC shows the frames) writes one row per
  frame shown ([kvm/video_stats.h](../common/include/kvm/video_stats.h)), and
  `video_stats_report run.csv "<content>"` turns that into the row below.
- **Latency:** from capture on the sending PC (its clock, mapped with the ping/pong offset) until
  the picture is on screen here. That is `UpdateLayeredWindowIndirect` in desk mode, or `EndDraw` in
  the window viewer.
- **Decode:** JPEG decode plus the alpha mask. **Present:** from decoded to on screen.
- **Dropped:** frame numbers that were never shown. Each desk-mode window counts on its own.
- The viewer's HUD (Ctrl+Alt+Shift+H) and a log line every 5 s show the same numbers for the last
  second.

**Loopback on this PC** ([tools/baseline_loopback.ps1](../tools/baseline_loopback.ps1)):
- **Setup:**
  - the real kvm_host and kvm_client on 127.0.0.1, in desk mode;
  - one 1280×800 window on the virtual display for 30 s;
  - JPEG quality 75 at 4:4:4, a 30 fps cap.
- **Caveat:** one CPU does both PCs' work (capture, encode, decode, compose), so this is a floor for
  the CPU cost, not a network figure.
- **Runs:** two, minutes apart. The second ran right after `ctest --preset local`.

| Date | Machine | Content | fps | Mbit/s | Latency p50 / p95 / p99 (ms) | Encode (ms, sender) | Decode avg (ms) | Present avg (ms) | Dropped |
|---|---|---|---|---|---|---|---|---|---|
| 2026-10-21 | Celeron laptop | static document (Notepad) | 29.9 / 28.2 | 71.4 / 67.2 | 50 / 73 / 82 · 70 / 98 / 140 | 17–18 | 14.5 / 17.2 | 7.7 / 7.9 | 0 / 2 |
| 2026-10-21 | Celeron laptop | browser scrolling (Edge) | 23.7 / 25.5 | 78.7 / 41.0 | 92 / 138 / 176 · 134 / 203 / 274 | 26–27 | 23.7 / 16.3 | 9.7 / 9.5 | 0 / 5 |
| 2026-10-21 | Celeron laptop | animation, 40 moving boxes (Edge canvas) | 27.9 / 23.8 | 27.8 / 23.8 | 76 / 111 / 145 · 87 / 132 / 155 | 22–23 | 16.8 / 18.0 | 9.8 / 10.0 | 0 / 0 |

**What it shows:**
- **Desk mode resends unchanged windows at the full frame rate.** A document that doesn't change
  costs 67–71 Mbit/s and a full JPEG encode, about 17 ms, 30 times a second. Sending only when the
  window changed, or H.264 (P2.3, where a still picture costs almost nothing), would remove most of
  that.
- **On this CPU, JPEG is most of the latency:**
  - encode 17–27 ms on the sender, decode 14–24 ms here, present 8–10 ms;
  - p50 from capture to screen is 50–134 ms depending on the content.
- **Scrolling and animation stay under the 30 fps cap** (24–26 fps): the encode, at 22–27 ms per
  picture, is the bottleneck.
- **The two runs differ by up to 40%:** what's on screen while scrolling, and background load.
  Compare changes with several runs, not one.

**Still to measure (needs two PCs):**
- home Wi-Fi and a phone hotspot, with the same content;
- video playback and a Photoshop or Blender window;
- the window viewer as well as desk mode.

## JPEG encode (hackathon baseline, for roadmap Phase 2)

| Date | Machine | Frame | Result | Source |
|---|---|---|---|---|
| 2026-10-20 | Celeron laptop | synthetic 1920×1080, quality 75 | avg 21.5 ms, p95 35.0 ms, 515 KB | `jpeg_test` (`bench` label) |
| 2026-10-17 | Temo's laptop | 1080p window | 4.2–5.3 ms | hackathon commit messages, not re-measured |
