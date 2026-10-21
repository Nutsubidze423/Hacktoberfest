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
