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

## BGRA to NV12 on the GPU (roadmap P2.3a)

**What:** `nv12_test` (label `gpu`) converts a 1920×1080 BGRA texture to NV12 with the D3D11 video
processor ([host_a/nv12_converter.h](../host_a/nv12_converter.h)), 60 times, then waits for the GPU:
the average per conversion, including the copy into the processor's input texture.

| Date | Machine | 1920×1080 BGRA → NV12 |
|---|---|---|
| 2026-10-21 | Celeron laptop (UHD 600) | 7.0 ms |

The same frame takes 17–27 ms to JPEG-encode on this CPU (above), and NV12 is only the input of
the hardware H.264 encoder (P2.3b), whose time is still to measure.

## H.264 encode on the GPU (roadmap P2.3b)

**What:** `h264_test` (label `gpu`):
- **Input:** 60 moving 1920×1080 pictures at 30 fps (a gradient and a moving block) go BGRA → NV12
  ([nv12_converter.h](../host_a/nv12_converter.h)), into a copy, and into the hardware encoder
  ([h264_encoder.h](../host_a/h264_encoder.h)): Intel Quick Sync, CBR at 8 Mbit/s, low latency, no
  B-frames.
- **Time:** from just before the conversion until the access unit came out.
- **Runs:** 5, back to back.

| Date | Machine | Time p50 (ms) | Time p95 (ms) | Bitrate (target 8.0 Mbit/s) | Dropped |
|---|---|---|---|---|---|
| 2026-10-21 | Celeron laptop (UHD 600) | 20.1–23.8 | 34.7–50.1, once 121.8 | 6.69–6.96 Mbit/s | 0, once 2 of 60 |

**What it means:**
- The CPU only queues work here, against a 17–27 ms JPEG encode on the CPU for a smaller,
  1280×800 window (above).
- The bitrate stays near the target, against desk mode's 24–79 Mbit/s with JPEG.
- Whether the picture holds up at that bitrate is P2.4's measurement, when the decoder can turn it
  back into pixels (PSNR).

## JPEG encode (hackathon baseline, for roadmap Phase 2)

| Date | Machine | Frame | Result | Source |
|---|---|---|---|---|
| 2026-10-20 | Celeron laptop | synthetic 1920×1080, quality 75 | avg 21.5 ms, p95 35.0 ms, 515 KB | `jpeg_test` (`bench` label) |
| 2026-10-17 | Temo's laptop | 1080p window | 4.2–5.3 ms | hackathon commit messages, not re-measured |

## Hardware codecs (roadmap P2.2)

**How:** `mft_probe --markdown` ([tools/mft_probe.cpp](../tools/mft_probe.cpp)) lists:
- every H.264 / HEVC / AV1 encoder and decoder that Media Foundation offers (`MFTEnumEx`), and
  what each takes;
- what the GPU itself decodes (`ID3D11VideoDevice` decode profiles, which Microsoft's decoder MFT
  uses through DXVA when it gets a D3D11 device).

Identical entries (one per graphics adapter, and a virtual display adds some) are listed once.

**Celeron laptop (Intel UHD Graphics 600), 2026-10-21:**

| Codec | Kind | Name | Hardware | Vendor | D3D11 | Async | Low latency | Raw formats | 4:4:4 format |
|---|---|---|---|---|---|---|---|---|---|
| H.264 | encoder | Intel® Quick Sync Video H.264 Encoder MFT | yes | Intel | yes | yes | yes | NV12 ARGB32 | - |
| H.264 | encoder | H264 Encoder MFT | no | - | no | no | yes | IYUV YV12 NV12 YUY2 | - |
| H.264 | decoder | Microsoft H264 Video Decoder MFT | no | - | yes | no | yes | NV12 YV12 IYUV I420 YUY2 | - |
| HEVC | encoder | Intel® Hardware H265 Encoder MFT | yes | Intel | yes | yes | yes | NV12 ARGB32 P010 | - |

| GPU | Decodes in hardware (D3D11 video) |
|---|---|
| Intel(R) UHD Graphics 600 | H.264, HEVC Main, HEVC Main10, VP9 |

**What it means for Phase 2:**
- **H.264 encode in hardware:** Intel Quick Sync, async, on D3D11 textures, with low-latency mode.
  That is what P2.3 needs for zero-copy encode from WGC.
- **H.264 decode in hardware:** the GPU decodes H.264 (and HEVC, VP9). Microsoft's H.264 decoder
  MFT works on D3D11, so P2.4 can decode on the GPU. This answers the open question about this
  laptop: it can be the weak PC in the demo with H.264.
- **HEVC:** encoded in hardware, and the GPU decodes it. But there is no HEVC decoder MFT: Windows
  needs the "HEVC Video Extensions" from the Store for that. HEVC (P2.10) can't be assumed on an
  ordinary PC.
- **AV1:** neither encode nor decode here.
- **4:4:4:** no encoder lists a 4:4:4 input (AYUV, Y410, Y416). ARGB32 input is converted to
  4:2:0 by the encoder, so P2.10's 4:4:4 isn't on this PC's table.
- **Still to probe:** the two demo PCs.
