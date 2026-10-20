# Sarke security: threat model and design

**Status:**
- This is a design, reviewed before any code ([roadmap](ROADMAP.md) task P1.1).
- Until Phase 1 ships, Sarke has **no authentication and no encryption**: anyone on the same
  local subnet can connect, see what is shared and control the PC. Use it only on networks you trust.

Decisions this design builds on ([ROADMAP.md](ROADMAP.md#decisions-needed-from-you)): OpenSSL 3
for TLS 1.3 (D6); pairing by comparing a 6-digit code on both screens (D7). Rule: **no hand-rolled
crypto**. Everything below uses TLS 1.3 and the OpenSSL primitives behind it as they are.

## Reporting a vulnerability

Report privately via GitHub: the repository's **Security** tab, then **Report a vulnerability**.
Please don't open a public issue for it.

## 1. What is worth protecting

| Asset | Why it matters |
|---|---|
| **Control of the PC** | The peer injects mouse and keyboard input (`SendInput`). Whoever controls the input controls the PC: it is as good as running code there. |
| **Keystrokes** | `key` messages carry everything typed while the cursor is on the other PC, passwords included. |
| **Clipboard** | Copied text (often passwords and tokens), images and file names. |
| **Files** | File contents sent across; the right to write into `Downloads\KVM`. |
| **Pixels** | Shared windows and desk mode stream what is on screen. |
| **Device identity** (after Phase 1) | Each PC's private key: whoever has it can pass as that PC to every PC it is paired with. |

## 2. Data flows today

| Port | What crosses it | Who may connect today |
|---|---|---|
| TCP 50500 | control and input: hello, keys, mouse, clipboard, layout | anyone; only `protocol_version` is checked ([host_a/main.cpp](../host_a/main.cpp) `handshake()`) |
| TCP 50501 | video frames (window pictures, desk mode) | anyone, at any time; the newest connection **replaces** the current one |
| TCP 50502 | files | anyone, at any time; writes into `Downloads\KVM` (paths are checked by `safe_destination`) |
| UDP 50503 | discovery (`kvm_discover` / `kvm_here`: name, port, version, busy) | anyone; replies are trusted by name for auto-connect |

The installer opens these ports to `remoteip=localsubnet` on **every** network profile, Public
included ([installer/sarke.iss](../installer/sarke.iss)).

## 3. Attackers

**In scope**

| ID | Attacker | Can |
|---|---|---|
| A1 | Passive, on the same Wi-Fi or LAN (café, campus, hotel, a shared flat) | Read all traffic. |
| A2 | Active, on the same network | Connect to any port; send forged discovery replies; ARP- or DHCP-spoof to sit in the middle; replay or modify traffic; flood ports. |
| A3 | A formerly paired device: lost, sold, or a friend's PC | Hold a valid key until it is unpaired. |
| A4 | A malicious peer that *is* paired | Sends crafted messages and files to exploit bugs in parsing or file handling. |

**Out of scope for v1.0**
- **The peer's input power:** a paired PC is trusted to control this one, so A4 gets no protection
  from that. Its power to break *out* of the protocol (bad paths, parser bugs) is in scope.
- **Malware running as the same Windows user:** it can already read the screen and inject input.
- **Traffic analysis:** sizes and timing of encrypted records reveal typing rhythm and screen activity.
- **Denial of service on the LAN beyond the pre-auth limits (S9):** anyone on the network can jam it anyway.
- **Physical access to an unlocked PC.**

## 4. Threats, today and after Phase 1

| # | Threat | Today | After Phase 1 |
|---|---|---|---|
| T1 | A2 connects to 50500 and controls the PC | possible (one hello) | refused: no paired key, no session (S1, S3) |
| T2 | A1 reads keystrokes, clipboard, files, pixels | possible (plain JSON/JPEG) | TLS 1.3 on every channel (S2) |
| T3 | A2 sits in the middle of a session | possible | both sides pin each other's key (S1) |
| T4 | A2 sits in the middle of *pairing* | n/a | the codes on the two screens differ; the user cancels (S5) |
| T5 | A2 takes over the video stream or pushes files (connects to 50501/50502) | possible; replaces the live channel | refused unless the same paired key **and** this session's token (S4) |
| T6 | A2 answers discovery with a paired PC's name, and auto-connect connects to it | possible, so A2 can then control this PC | discovery is only a hint; the TLS key decides (S6) |
| T7 | A3 keeps access after "unpair" | n/a | unpair deletes the key and ends the session at once (S8) |
| T8 | A4 sends a path that escapes `Downloads\KVM` | blocked by `safe_destination` (tested) | unchanged, and only after authentication |
| T9 | A4 or A2 sends a huge or malformed message | size limits (64 KB control, 32 MB frame) | the same limits, reachable only after authentication; decoder fuzzed (P1.12) |
| T10 | A2 exhausts connections before authentication | possible | handshake timeout, a cap on pending handshakes, per-IP backoff (S9) |
| T11 | Another user or a backup reads the private key | n/a | DPAPI, current user (S7) |
| T12 | Public Wi-Fi: anyone on it can reach the ports | yes (`profile=any`) | firewall rules for Private (and Domain) networks only (P1.11) |

## 5. Required properties

| ID | Property |
|---|---|
| S1 | **Mutual authentication.** Each PC proves its device key in the TLS handshake, and the other side accepts only keys it paired with. |
| S2 | **Confidentiality and integrity** of every channel (50500, 50501, 50502, and later audio): TLS 1.3 only. |
| S3 | **Nothing from an unpaired peer is parsed.** No application byte is read until TLS is done and the key is checked. The only exception is the pairing exchange, which handles only pairing messages. |
| S4 | **Side channels belong to the session.** Video and file connections must present the same key as the live control session, plus its one-time token. They never replace a channel of the live session. |
| S5 | **Pairing resists a man-in-the-middle.** Each side's code is derived from its own TLS session, so a relay ends up with different codes on the two screens. |
| S6 | **Discovery is a hint.** A discovery reply can only cause a connection attempt; it can't grant trust. |
| S7 | **The private key is protected at rest:** DPAPI, current user. |
| S8 | **Unpair takes effect immediately:** the key is removed and any live session with it ends. |
| S9 | **Pre-auth limits:** a 10 s handshake timeout, at most 4 pending handshakes, and per-IP backoff after a failed handshake. |

## 6. Design

### 6.1 Device identity
- **The key:** generated on the first start, one ECDSA P-256 key pair per Windows user per PC, with
  OpenSSL `EVP_PKEY_Q_keygen`. P-256 rather than Ed25519 keeps future macOS and phone clients easy.
- **The certificate:** self-signed X.509, Subject CN = the PC's name (display only, never trusted),
  serial random, valid 100 years. Nobody checks the dates: trust comes from pinning, not from a CA.
- **The device ID** is the SHA-256 of the certificate's DER-encoded SubjectPublicKeyInfo. It is
  shown to people as 4 groups of 4 hex digits (the first 8 bytes).
- **Storage:** `identity.bin` in the config folder (`%LOCALAPPDATA%\Sarke` when installed). It holds
  the PKCS#8 private key and the certificate, encrypted with `CryptProtectData` (current user, no
  UI).
- **A blob that can't be decrypted or parsed is never replaced silently.** Sarke stops, tells the
  user, and creates a new key only when they agree, which means pairing again.

### 6.2 TLS profile (OpenSSL 3, all three TCP channels)
- **Version:** TLS 1.3 only (`SSL_CTX_set_min_proto_version(TLS1_3_VERSION)`).
- **Cipher suites and groups:** OpenSSL's defaults for TLS 1.3 (AES-GCM and ChaCha20-Poly1305;
  X25519 and P-256 key exchange).
- **Certificates on both sides:**
  - The server requests the client's certificate and requires it (`SSL_VERIFY_PEER |
    SSL_VERIFY_FAIL_IF_NO_PEER_CERT`).
  - The verify callback accepts any well-formed certificate. Pinning happens right after the
    handshake (6.4), not in the callback, so a failure can be reported clearly.
- **Turned off:** session resumption and 0-RTT early data, so every connection is a full
  handshake bound to the keys:
  - `SSL_CTX_set_num_tickets(ctx, 0)`;
  - `SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF)`;
  - early data stays at OpenSSL's default of 0.
- **Socket:** `TCP_NODELAY` stays on.
- **Implementation:** `TlsStream` wraps a connected socket and keeps the current blocking
  `send_all` / `recv_exact` semantics ([common/src/socket_util.h](../common/src/socket_util.h)).

### 6.3 Pairing (D7: compare a code on both screens)
1. **Start:** the user picks an unpaired PC in the tray ("Pair…"). A connects to B on 50500. The
   TLS handshake completes with both certificates, but neither key is in the other's
   `paired.json`.
2. **The pairing state:** both sides see an unknown key and enter **pairing state**. In it, only
   `pair_request`, `pair_confirm` and `pair_result` are accepted; anything else closes the
   connection. Input, clipboard, files and video are never set up in this state.
3. **The code:** both derive it from their own TLS session:
   ```
   context  = fingerprint_low || fingerprint_high      (the two 32-byte device IDs, the smaller first)
   label    = "EXPORTER-sarke-pairing-v1"
   material = SSL_export_keying_material(ssl, out, 4, label, strlen(label), context, 64, /*use_context=*/1)
   code     = big_endian_u32(material) mod 1 000 000, shown as two groups of three digits: "042 917"
   ```
   It is the RFC 8446 §7.5 exporter. A man-in-the-middle has two different TLS sessions, so the
   two screens show different codes, except with probability 10⁻⁶ per attempt.
4. **Confirming:** each PC shows "Pair with *Name*? Both screens show **042 917**", with
   Confirm/Cancel and a 60 s timeout. Each side sends `pair_confirm{ok}` with its user's answer.
5. **The result:** only when **both** confirmed does each side store the other's device ID, name
   and default permissions in `paired.json` and send `pair_result{ok: true}`. Then the session
   continues as a normal paired session.
6. **No:** a cancel, a timeout or a closed connection on either side stores nothing.
7. **Attempt limit:** at most 3 pairing attempts per peer IP per 10 minutes. With 10⁻⁶ per attempt,
   guessing past the users is impractical.

### 6.4 Paired sessions
- **Pinning:** right after the TLS handshake, before reading one application byte:
  - the peer's device ID is looked up in `paired.json`;
  - not found → pairing state (only if this PC started the pairing or the user accepts an incoming
    one); otherwise the connection is closed and "*IP* tried to connect with an unknown key" is logged.
- **The `hello` exchange** happens over TLS as today, plus `device_id` for the logs. The
  `protocol_version` becomes 3.
- **The session token:** the host draws 32 random bytes (`RAND_bytes`) per session and sends them
  as `session_token{token}` on the control channel.
- **Side channels** (P1.7; from P0.10 the host's answer also carries `video_port` and `file_port`):
  - the client connects 50501 and 50502 only after `session_token`;
  - each runs TLS with the same certificates, and its first header carries `token`;
  - the host accepts one only if the peer's device ID equals the control session's peer **and**
    the token matches (`CRYPTO_memcmp`);
  - otherwise it closes it without touching the live channel;
  - acceptors run only while a session is live.

### 6.5 Discovery
`kvm_here` adds `device_id`. Auto-connect matches `device_id` against `paired.json` (`auto_connect:
true`); the name is only shown. A reply claiming a paired name with another ID is shown as a
separate, unpaired PC. Discovery stays unauthenticated on purpose: it can only cause a TLS attempt,
which then has to pass pinning.

### 6.6 Paired-device store
`paired.json` in the config folder: `[{device_id, name, paired_at, last_seen, auto_connect,
permissions}]`. It holds public data only, so it isn't encrypted. Anyone who can write it already
runs as this user (out of scope). **Unpair:** the entry is removed and any live session with that
device ID is closed. The other side's next attempt fails pinning and shows "*Name* unpaired this PC".

### 6.7 Version mismatch
A v2 peer starts with plaintext `[u32 len]{"type":"hello"…` where TLS expects a ClientHello
(first byte `0x16`). The v3 server peeks at the first byte. If it isn't `0x16`, it closes the
connection and logs "an older Sarke (protocol v2) at *IP* tried to connect; update both PCs". The
launcher shows that as a toast. Discovery's `protocol_version` already lets the tray mark an older
PC before any connection.

### 6.8 What never goes into logs
- Key contents, clipboard contents, file contents, session tokens and private keys are never logged.
- Allowed: sizes, kinds, device IDs (short form), IPs and file names. Logs stay in the user's
  profile.

## 7. Residual risks (accepted for v1.0)

| Risk | Why accepted |
|---|---|
| A paired PC has full control (A4, by design) | That is the product: unpair a PC you don't trust. Per-device permissions in Phase 4 narrow it. |
| Traffic analysis reveals typing rhythm | Padding the input stream costs latency. Revisit for internet use (Phase 6). |
| A Windows reinstall or profile reset loses the key | You pair again. This is documented. |
| DPAPI protects against other users, not against malware as this user | Out of scope (section 3). |
| One code comparison by people who might not look closely | 6 digits plus the "both must confirm" step. The UI makes the codes large and asks "do they match?" rather than "OK?". |
| Anyone on the LAN can make this PC show a pairing question, hoping its user clicks Pair | The question says to pair only if *you* started pairing on the other PC and it shows the same code. Cancel is the default button. It cancels itself after 55 s. Each IP gets 3 attempts per 10 minutes. |

## 8. How it is tested

| Property | Test |
|---|---|
| S1, S3 | `loopback_test`: a peer with an unknown key is closed before any `hello` log line; a peer with a paired key connects (P1.5) |
| S2 | `tls_test`: TLS 1.2 refused, a plaintext peer refused. A packet capture during the two-PC regression run shows no readable JSON or JPEG (P1.14) |
| S4 | `loopback_test`: a second 50501/50502 connection with another key, or the right key and a wrong token, is refused and doesn't replace the live channel (P1.7) |
| S5 | `tls_test`: a relaying man-in-the-middle with its own key gives different exporter values; the code mapping matches the vectors below (P1.2, P1.6) |
| S6 | launcher unit test: a reply with a paired name but another ID isn't auto-connected (P1.8) |
| S7 | `identity_test`: round trip; a tampered blob fails cleanly. Manual: another Windows user can't decrypt it (P1.3) |
| S8 | `loopback_test`: unpair mid-session closes all three channels (P1.9) |
| S9 | `loopback_test`: a flood of half-open connections can't exhaust the slots, and the handshake times out (P1.12) |

### Code mapping test vectors
The mapping from 4 exporter bytes to the code shown:

| Exporter bytes (hex) | u32 | Code shown |
|---|---|---|
| `00 00 00 00` | 0 | `000 000` |
| `00 0F 42 3F` | 999 999 | `999 999` |
| `00 0F 42 40` | 1 000 000 | `000 000` |
| `12 34 56 78` | 305 419 896 | `419 896` |
| `FF FF FF FF` | 4 294 967 295 | `967 295` |

(2³² isn't a multiple of 10⁶, so codes 000 000–967 295 are a little more likely than the others:
4 295 versus 4 294 chances in 2³², about +0.0008 %. That's negligible next to 10⁻⁶ per attempt.)
