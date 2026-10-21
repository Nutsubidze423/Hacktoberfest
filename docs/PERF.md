# Sarke performance measurements

Every number Sarke's docs, README or PRs claim comes from this page, together with how it was
measured and on what hardware. Older numbers stay as history. Roadmap task P2.1 adds the
end-to-end video latency baseline.

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
