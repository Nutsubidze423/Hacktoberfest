# გუნდის გეგმა — KVM + Window Streaming (C++)

4 დეველოპერი, თითოეული მუშაობს Claude-თან ერთად (vibe coding).
დემოს ფარგლები, პროტოკოლი და წესები — [CLAUDE.md](../CLAUDE.md). ეს ფაილი აღწერს **ვინ რას აკეთებს და რა თანმიმდევრობით**.

## ძირითადი პრინციპი
- თითოეულს აქვს თავისი **ტრეკი** და თავისი **ფაილები**. სხვის ფაილს არ ეხები → merge კონფლიქტები თითქმის არ იქნება.
- ერთადერთი საერთო კონტრაქტი არის `common/include/kvm/protocol.h`. ის **პირველ საათში იყინება** და მერე იცვლება მხოლოდ ლუკას PR-ით.
- ყველა ტრეკი ჯერ **დამოუკიდებლად** მუშაობს და ტესტდება (ქსელის ან სხვისი კოდის გარეშე), მერე ვაერთებთ.

> **17.10.2026, ლუკა:** ყველა ფუნქცია ორივე მიმართულებით (A ⇄ B) — KVM, ფანჯარა, ფაილები; protocol v2. ახალი დავალებები (S1–S3, T8–T9, D0–D2): `ARTEFACT/გადაწყვეტილებები 2 (ლუკა → თემო, სანდრო, დემეტრე).md`. თითოეულმა თავისი სექცია ამის მიხედვით განაახლოს.

## როლები

| | როლი | ფლობს | მთავარი შედეგი |
|---|---|---|---|
| **ლუკა** | Core / Net / ინტეგრაცია (lead) | root CMake, `vcpkg.json`, `common/` (protocol, net, config, log, queue), `host_a/main.cpp`, `client_b/main.cpp`, `tools/ping_test.cpp` | კავშირი, ხელახალი დაკავშირება, აპების „ჩონჩხი“, PR review და merge |
| **დემეტრე** | KVM ინფუთი | `common/win32_input.*`, `common/dpi.*`, `host_a/input_capture.*`, `client_b/input_inject.*` | A-ს მაუსი/კლავიატურა გადადის B-ზე; გადაუდებელი ღილაკი |
| **თემო** | ვიდეო A მხარეს | `host_a/window_picker.*`, `host_a/capture.*`, `host_a/jpeg_encoder.*`, `host_a/video_sender.*`, `tools/capture_test.cpp` | ფანჯრის capture → JPEG → ქსელი |
| **სანდრო** | Viewer B მხარეს + უკუ-ინფუთი | `client_b/viewer_window.*`, `client_b/jpeg_decoder.*`, `client_b/video_receiver.*`, `host_a/remote_input.*`, `tools/fake_streamer.cpp` | სტრიმის ჩვენება B-ზე, კლიკები/კლავიატურა → A-ს ფანჯარა |

ლუკა ყველაზე ადრე ასრულებს თავის ძირითად ნაწილს — მერე ეხმარება იმას, ვინც ჩამორჩება (ყველაზე სავარაუდოდ თემო — capture ყველაზე რთულია).

## ტექნიკური გადაწყვეტილებები (დემომდე არ ვცვლით)
- **C++20**, MSVC (Visual Studio 2022 / Build Tools), **CMake ≥ 3.25 + Ninja**, **vcpkg** manifest mode.
- დამოკიდებულებები: `nlohmann-json`, `libjpeg-turbo`. C++/WinRT და D3D11 მოდის Windows SDK-სთან ერთად (≥ 10.0.19041).
- **ქსელი:** Winsock2, ბლოკირებადი სოკეტები + ცალკე thread-ები (recv / send). ასინქრონული ბიბლიოთეკა არ გვჭირდება.
- **Thread-ებს შორის:** `kvm::BlockingQueue<T>` (control/input) და `kvm::LatestSlot<T>` (ვიდეო — ინახავს მხოლოდ ბოლო ფრეიმს). ორივეს წერს ლუკა `common/include/kvm/queue.h`-ში.
- **A-ს hooks:** `WH_MOUSE_LL` + `WH_KEYBOARD_LL` ცალკე thread-ზე message loop-ით. როცა კონტროლი B-ზეა, hook აბრუნებს `1` (suppress), ხოლო relative მოძრაობას ვკითხულობთ **Raw Input**-ით (`WM_INPUT`, `RIDEV_INPUTSINK`) — ასე კურსორის ცენტრში დაბრუნება არ გვჭირდება.
- **Capture:** Windows.Graphics.Capture (C++/WinRT) → D3D11 staging texture → CPU BGRA.
- **Encode v1:** TurboJPEG. **v2 (თუ დრო დარჩა):** Media Foundation H.264 hardware MFT — მუშაობს NVIDIA / AMD / Intel-ზე.
- **Viewer v1:** Win32 ფანჯარა + `StretchDIBits` (უმარტივესი). **v2:** D3D11 swapchain.
- **ვიდეოს ფრეიმი 50501 პორტზე:** `[u32 header_len][u32 payload_len][header JSON][payload]`, big-endian. header: `{"type":"frame","seq","t_capture_us","width","height","codec":"jpeg"}`.
- **Latency გაზომვა:** `pong` აბრუნებს `t_peer`-საც → ლუკა ითვლის საათების სხვაობას (clock offset) და აძლევს viewer-ს.

## ვადები (≈10 სამუშაო საათი)

### M0 — სთ 0–1: ყველა ერთად
- **ერთი დღით ადრე ყველამ:** VS 2022 (Desktop C++ workload) + Windows SDK, Git, vcpkg (`VCPKG_ROOT`). ეს ~1 საათია — ნუ დატოვებთ დემოს დღისთვის.
- ლუკა: repo-ს ჩონჩხი — CMake, presets, vcpkg, ცარიელი `kvm_host.exe` / `kvm_client.exe`, `protocol.h`.
- ყველა: ააწყოს build საკუთარ კომპიუტერზე; ერთად გადახედეთ `protocol.h`-ს და **გაყინეთ**.
- ✅ **M0 მზადაა, როცა:** 4-ივე კომპიუტერზე `cmake --build` გადის და `protocol.h` main-შია.

### ფაზა 1 — სთ 1–5: პარალელურად, თითოეული დამოუკიდებლად

**ლუკა — Core / Net**
1. `net.h/.cpp`: `Connection` კლასი — length-prefixed JSON send/recv, `TCP_NODELAY`, recv thread → queue.
2. `Server` (A) / `Client` (B, ხელახალი დაკავშირება ყოველ 1 წმ-ში).
3. `hello`, `ping`/`pong` + clock offset; `config.h` (config.json-ის წაკითხვა, CLI `--host`).
4. `tools/ping_test.cpp` (Python-ის ვერსიის ჩანაცვლება); `requirements.txt` წაშლა.
5. ვიდეოს framing (`send_frame` / `recv_frame`) 50501 პორტზე.
- ✅ ტესტი: ერთ კომპიუტერზე `127.0.0.1` — host + client, hello ჩანს ლოგში, ping < 1 ms; client-ის დახურვა/გახსნა → თავიდან უკავშირდება.

**დემეტრე — KVM ინფუთი**
1. `win32_input.*`: `send_mouse_move_rel`, `send_mouse_button`, `send_wheel`, `send_key(vk, scan, down, extended)`, `move_cursor_abs`. (სანდრო იყენებს — **პირველ 1.5 საათში** დაამერჯე.)
2. `dpi.*`: `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`, მონიტორების სია, ვირტუალური დესკტოპის კიდე.
3. `input_capture.*`: hooks thread, **ჯერ** `Ctrl+Alt+Shift+Q`, მერე edge detection (`client_side` config-დან), suppress, Raw Input deltas → callback/queue.
4. `input_inject.*` (B): მიღებული შეტყობინებები → `SendInput`; `focus_enter`-ზე კურსორი კიდეზე `y_norm`-ით; B-ს მოპირდაპირე კიდიდან `focus_leave` უკან.
- ✅ ტესტი (ქსელის გარეშე): `kvm_host.exe --local-test` — ლოგში ჩანს `dx/dy`, კლავიშები; კიდეზე გასვლა ბლოკავს ლოკალურ ინფუთს; `Ctrl+Alt+Shift+Q` ყოველთვის აბრუნებს.

**თემო — ვიდეო A**
1. `window_picker.*`: ხილული top-level ფანჯრების სია (title, HWND, process), არჩევა კონსოლში ნომრით.
2. `capture.*`: WGC session HWND-ისთვის, `FramePool` callback → staging texture → BGRA buffer; ფანჯრის ზომის ცვლილების დამუშავება.
3. `tools/capture_test.cpp`: არჩეული ფანჯრის 10 ფრეიმი → `captures/*.jpg`.
4. `jpeg_encoder.*`: TurboJPEG, ხარისხი config-დან; გაზომე ms/ფრეიმი.
- ✅ ტესტი: `capture_test.exe` ინახავს სწორ JPEG-ებს; encode 1080p < 15 ms.

**სანდრო — Viewer B**
1. `jpeg_decoder.*`: TurboJPEG → BGRA.
2. `viewer_window.*`: Win32 ფანჯარა, `StretchDIBits`, aspect ratio შენარჩუნება, FPS/latency overlay.
3. `tools/fake_streamer.cpp`: საქაღალდიდან JPEG-ებს აგზავნის 50501-ზე (ლუკას framing-ით; სანამ მზად არ არის — ჯერ ფაილიდან პირდაპირ viewer-ში).
4. viewer-ის ინფუთი: `WM_MOUSEMOVE/BUTTON/WHEEL/KEY` → ნორმალიზებული `remote_*` / `key` შეტყობინებები (ჯერ ლოგში).
- ✅ ტესტი: `fake_streamer` + `kvm_client` ერთ კომპიუტერზე — სურათი ჩანს, FPS ჩანს, კლიკებზე ლოგში სწორი `x_norm/y_norm`.

### M1 — სთ 5–6: პირველი ინტეგრაცია (ფიზიკური A + B)
- ლუკა + დემეტრე: KVM ქსელით — მაუსი და კლავიატურა A → B, კიდიდან გადასვლა და დაბრუნება.
- ლუკა + თემო + სანდრო: `video_sender` → `video_receiver` — რეალური ფანჯრის სტრიმი B-ზე.
- ✅ **M1:** ეს ორი სცენარი მუშაობს ერთდროულად; `main`-ში დამერჯილია.

### ფაზა 2 — სთ 6–9
- **სანდრო:** `host_a/remote_input.*` — `x_norm/y_norm` → ფანჯრის **client area**-ს მიმდინარე rect → `SendInput`; ფოკუსი (`SetForegroundWindow` + `AttachThreadInput` ხრიკი); კლავიატურა.
- **დემეტრე:** სქროლი (ჰორიზონტალურიც), extended კლავიშები (ისრები, Win, AltGr), გაჭედილი კლავიშების გასუფთავება `focus_leave`-ზე; viewer-ზე ფოკუსისას KVM-ის ქცევა.
- **თემო:** `stream_start/stop`, ფანჯრის ზომის ცვლილება სტრიმის დროს, max FPS შეზღუდვა, მხოლოდ ბოლო ფრეიმის გაგზავნა. თუ დრო დარჩა: H.264 (Media Foundation).
- **ლუკა:** end-to-end latency გაზომვა, ლოგები, კავშირის გაწყვეტის სცენარები; ეხმარება ჩამორჩენილ ტრეკს.

### M2 — სთ 9–10: დემო
- Photoshop-ით ტესტი: ფუნჯით ხატვა B-დან, სქროლი, კლავიატურის შორთქათები.
- JPEG ხარისხის / FPS-ის მორგება ქსელზე. **Hotspot-ზე ნუ გააკეთებთ დემოს** — Wi-Fi რაუტერი ან კაბელი.
- დემოს სცენარის 2-ჯერ გავლა; ცნობილი პრობლემების სია README-ში.

## Git workflow
- ბრენჩი თითო ამოცანაზე: `demetre/hooks-edge`, `temo/wgc-capture`...
- PR პატარა და ხშირი — **მინიმუმ ყოველ 2 საათში ერთხელ** main-ში. ლუკა აკეთებს review-ს და merge-ს.
- PR-მდე: `git pull --rebase origin main`, build გადის, საკუთარი ტესტი გადის.
- სხვისი ფაილის შეცვლა გჭირდება? → ჰკითხე მფლობელს; `protocol.h` — მხოლოდ ლუკა.
- Commit-ები ინგლისურად.

## Claude-თან მუშაობის წესები (vibe coding)
1. სესიის დასაწყისში მისწერე Claude-ს **„მე ვარ <სახელი>“** (იხ. ქვემოთ). Claude თავისით კითხულობს CLAUDE.md-ს.
2. **ერთი ამოცანა = ერთი სესია = ერთი commit.** დიდ ამოცანას Claude-ს სთხოვე, ჯერ დაყოს.
3. სთხოვე Claude-ს, რომ **ააწყოს და გაუშვას** ტესტი, სანამ „მზადაა“ იტყვის. build-ის შეცდომას ნუ დაამერჯებ.
4. **წაიკითხე ყველა diff.** რაც არ გესმის — ჰკითხე Claude-ს, ახსნას. რაც ვერ გაიგე, იმას ნუ დაამერჯებ.
5. Claude-მ სხვის ფაილებს არ უნდა შეეხოს — თუ შესთავაზა, უარი უთხარი და მფლობელს მისწერე.
6. Hook-ებთან და ინფუთის ბლოკვასთან მუშაობისას — **ჯერ გადაუდებელი ღილაკი**, მერე დანარჩენი. გაჭედვისას: `Ctrl+Alt+Shift+Q`, შემდეგ `Ctrl+Alt+Del`.
7. თუ Claude 2–3 მცდელობით ვერ ასწორებს ერთსა და იმავე შეცდომას — შეჩერდი, ახალი სესია გახსენი, ან ლუკას მიმართე.

## როგორ დაიწყო სესია
უბრალოდ მისწერე Claude-ს: **„მე ვარ ლუკა“** (ან დემეტრე / თემო / სანდრო).
Claude წაიკითხავს ამ ფაილს, იპოვის შენს ტრეკს და ქვემოთ შენს ჩეკლისტში პირველ მოუნიშნავ პუნქტს — და იქიდან გააგრძელებს.
ამოცანის დასრულებისას Claude მონიშნავს პუნქტს (`[x]`) იმავე commit-ში.

## პროგრესი

### ლუკა — Core / Net / ინტეგრაცია
- [x] M0: CMake + presets + vcpkg, ცარიელი `kvm_host` / `kvm_client`, `.gitignore` (build/), `tools/setup_env.ps1`
- [ ] M0: `protocol.h` — ყველა შეტყობინება, გუნდთან ერთად გაყინული (დაწერილია + `tools/protocol_test.cpp`; რჩება გუნდის review)
- [x] `queue.h` (`BlockingQueue`, `LatestSlot`) + `tools/queue_test.cpp`, `log.h`, `config.h`
- [x] `net.*`: `Connection`, `Server`, `connect()` + ხელახალი დაკავშირება (B: ყოველ 1 წმ-ში), dead-link 5 წმ
- [x] `hello` (+ ვერსიის შემოწმება), `ping`/`pong` + clock offset (min-RTT, NTP-ს მსგავსად)
- [x] ping ტესტი — `kvm_host`/`kvm_client` ყოველ 5 წმ-ში წერენ rtt-ს; Python ფაილები წაშლილია
- [x] ვიდეოს არხი 50501: `video.h` (`FrameChannel`, `VideoServer`, `connect_video`) + `tools/video_test.cpp`; `kvm_host --test-video` (სატესტო სტრიმი, B ლოგავს fps/Mbit/latency)
- [x] M1a: KVM ინტეგრაცია დემეტრესთან — ტესტი 2 გავიდა ორ კომპიუტერზე (`4d16b70`, touchpad-ის ჩათვლით)
- [ ] ტესტი 3: სატესტო ვიდეო ნამდვილ ქსელზე (`5_run_host_video_test.cmd`) — fps / Mbit/s / latency
- [x] protocol v2 (ორმხრივი), ორივე exe ორივე ბიბლიოთეკას ლინკავს, A-ს პასუხის `hello`, firewall 50502
- [x] M1b: ფანჯარა A ⇄ B `main.cpp`-ში: ვიდეო არხი ორივე მიმართულებით, `WindowShare` + viewer + `remote_input`, hotkey `Ctrl+Alt+Shift+W`, დაბრუნება გამოსვლისას — გააკეთა თემომ გუნდის თხოვნით (`host_a/window_link.*`, სანდროს viewer-ის მერჯი); ერთ PC-ზე შემოწმებულია, ორ PC-ზე ჯერ არა
- [x] KVM ორივე მიმართულებით + clipboard `main.cpp`-ში (დემეტრეს `KvmLink`, `ClipboardSync`), protocol `clipboard` (`0d7a7c1`)
- [x] ფაილების არხი 50502 `main.cpp`-ში (თემოს T9) + `FileLink` + `Hotkeys` (`Ctrl+Alt+Shift+F` = drop ყუთი კურსორთან, სანამ დემეტრეს D1 არ არის)
- [x] ავტომატური ძებნა ქსელში (UDP 50503): `"host_ip": "auto"`, B HOST-ს სახელით პოულობს და უკავშირდება; IP-ს შეცვლისას თავიდან პოულობს
- [ ] ფაზა 2: end-to-end latency, კავშირის გაწყვეტის სცენარები
- [ ] M2: დემოს სცენარი, README

### დემეტრე — KVM ინფუთი
- [x] `win32_input.*` (SendInput wrappers) — **სანდროს სჭირდება, პირველ რიგში**
- [x] `dpi.*`: DPI awareness, მონიტორები, ვირტუალური დესკტოპის კიდეები (+ `tools/dpi_test.cpp`)
- [x] `input_capture.*`: hooks thread + `Ctrl+Alt+Shift+Q` (+ `tools/input_capture_test.cpp`)
- [x] edge detection + suppress + Raw Input deltas; local test = `tools/kvm_local_test.exe` (`kvm_host.exe`-ს არ ეხება)
- [x] `input_inject.*` (B): მოძრაობა, ღილაკები, `focus_enter` / `focus_leave` (+ `tools/input_inject_test.cpp`)
- [x] M1: მაუსი + კლავიატურა A → B ქსელით (ტესტი 2, `4d16b70`; სიგლუვე `8461a31`)
- [ ] ფაზა 2: სქროლი (ჰორიზ. ჩათვლით), extended კლავიშები, გაჭედილი კლავიშების გასუფთავება

### თემო — ფანჯარა A ⇄ B (VDD + ვიდეო owner-ის მხარეს) და ფაილები
დეტალები: `ARTEFACT/თემო — დავალებები (ლუკა → თემო).md`, `ARTEFACT/გადაწყვეტილებები 2 (ლუკა → თემო, სანდრო, დემეტრე).md`.
- [x] `window_picker.*`: ფანჯრების სია და არჩევა
- [x] T1: `capture_test` — `#include` .cpp-ის ნაცვლად `kvm_host_lib`
- [x] T2: `virtual_display.*`: პოვნა, რეჟიმი, მხარე, attach/detach + `tools/vdd_test.cpp` (თემოს ლეპტოპზე გადის; A-ზე ლუკამ გაუშვას)
- [x] T3: `window_mover.*`: ფანჯარა VDD-ზე (maximize) და უკან (+ `tools/window_mover_test.cpp`)
- [x] T4: `capture.*`: WGC `CreateForMonitor` → BGRA → `LatestSlot` (64 fps, WGC → consumer ≤ 8 ms თემოს ლეპტოპზე)
- [x] T5: `jpeg_encoder.*`: TurboJPEG + დროის გაზომვა (`tools/jpeg_test.exe`; თემოს ლეპტოპზე 1920×1080: 4.2 ms, 69 KB რეალურ ეკრანზე)
- [x] T6: `capture_test`: ფანჯარა VDD-ზე → 10 ფრეიმი `captures/*.jpg` → უკან (`--menu`-ით მენიუც ჩანს)
- [x] T7: `video_sender.*`: `LatestSlot` → `FrameChannel::send_frame` (ლუკას `video.h`); `tools/video_sender_test.exe` გადის, მათ შორის TCP-ზე (30 fps, capture → B ~25 ms loopback-ზე)
- [x] `window_share.*`: გაზიარების მთელი owner-მხარე ერთ ობიექტში (VDD → ფანჯარა → capture → sender, უკან სწორი თანმიმდევრობით; `tools/window_share_test.exe`)
- [x] T8: `remote_input.*`: viewer-ის `remote_*`/`key` → VDD-ზე `SendInput` (`tools/remote_input_test.exe`: Paint-ში ხაზი და Ctrl+Z, სათაურზე კლიკის შემდეგაც მაქსიმალებული რჩება)
- [x] T9: ფაილები A ⇄ B: `common/file_transfer.*` (50502) + `common/file_drop.*` — `tools/file_transfer_test.exe` (ორივე მიმართულებით, უსაფრთხოება), `tools/file_drop_test.exe`; ნამდვილი OLE drop: `tools/file_drag_test.exe`
- [x] T10: ფაილის გადათრევა ორ კომპიუტერს შორის (Mouse Without Borders-ის მსგავსად, ნებისმიერ საქაღალდეში): `common/file_drag.*` (drag-ის გაგრძელება მიმღებ მხარეს), `FileDropCatcher::catch_now`, `send(..., drag)` → `on_drag_received`; `tools/file_drag_test.exe` მთელ ნაკადს ერთ PC-ზე ამოწმებს. ჩართვა: დემეტრე (KVM გადასვლა drag-ის დროს) + ლუკა (`main.cpp`)
- [x] T11 (ერთ PC-ზე): **desk mode** — მეორე PC-ის ეკრანი = კიდევ ერთი მონიტორი. ფანჯარას კიდეზე გადაათრევ, owner-ზე რჩება VDD-ზე (peer-ის ზომით, მასშტაბით და მხარეს) და peer თავის desktop-ზე იმავე ადგილას ხატავს; კლიკი ფანჯარაზე native-ია, დანარჩენი peer-ს ეგზავნება, კლავიატურა ბოლო კლიკს მიჰყვება; peer-ის საკუთარი მაუსიც მუშაობს ამ ფანჯრებზე. ფაილები: `common/desk_region.*`, `host_a/desk_tracker.* desk_share.* desk_input.* frame_router.*`, `client_b/desk_view.*`; ცვლილებები: protocol (`desk_move`, `desk_cursor`, `hello/focus_enter.desk`, frame `layer/rects`), `input_capture`/`input_inject`/`kvm_link` (დემეტრეს), `main.cpp` (ლუკას) — თემოს თხოვნით. ტესტები: `tools/desk_test.exe --auto`, `tools/desk_hook_test.exe`, ძველები ყველა გადის
- [x] T11 polish (პირველი ორ PC-იანი ტესტის მერე): თითო ფანჯარა ცალკე WGC capture-ით + alpha (pixel-perfect, ფონის გარეშე, მომრგვალებული კუთხეები), მდებარეობა `desk_layout`-ით (გადათრევა სურათის გარეშე, ციმციმის გარეშე); drag-ის ხატულა აღარ იხატება; მეორე ეკრანიდან გახსნილი აპი იქვე ჩნდება; მეორე PC-ის მაუსი აღარ წაართმევს კურსორს მოძრავ მომხმარებელს; კურსორი აღარ იჭედება კიდის იმ ნაწილზე, სადაც VDD მოკლეა
- [ ] T11 ორ PC-ზე (თემო + სანდრო): YouTube A → B და უკან, B → A (სანდროს VDD სჭირდება)
- [ ] M1: რეალური ფანჯრის სტრიმი B-ზე (ლუკასთან და სანდროსთან ერთად)
- [ ] ფაზა 2: (სურვ.) H.264 Media Foundation

### სანდრო — Viewer (ორივე მხარეს) + უკუ-ინფუთი
დეტალები: `ARTEFACT/გადაწყვეტილებები 2 (ლუკა → თემო, სანდრო, დემეტრე).md` (S1–S3), პასუხი: `ARTEFACT/სანდრო — პასუხი და viewer-ის ჩართვა (სანდრო → თემო, ლუკა).md`.
- [x] `jpeg_decoder.*`: TurboJPEG → BGRA (1080p decode ≈ 7 ms; ტესტი: `tools/viewer_test.cpp`)
- [x] `viewer_window.*`: Direct2D (`docs/DESIGN.md` 4): aspect ratio, სათაურის ზოლი, მდგომარეობის ეკრანები, HUD (FPS/LAT/RTT/MBPS…)
- [x] `tools/fake_streamer.cpp` (`VideoServer`-ით) + `viewer_test.exe --connect <host>`
- [x] S1: `video_receiver.*` კითხულობს გარედან მიცემულ `FrameChannel&`-ს (არხს და reconnect-ს `main.cpp` ფლობს); LatestSlot, 1080p 30 fps / ~9 ms ლოკალურად; `kvm_host --no-kvm --test-video`-ის ფრეიმები მიიღო (codec `test` არ ჩანს, MBPS-ში ითვლება)
- [x] S3: viewer-ის ინფუთი → `remote_mouse_move`, `mouse_button`, `mouse_wheel`, `key` (`viewer_input.*`, ორივე exe-ში; მხოლოდ გამოსახულებაზე, drag-ისას clamp, ფოკუსის დაკარგვისას ყველაფერი up, Alt+F4 ლოკალურია). viewer-ის დახურვა სტრიმისას → `stream_stop {hwnd}`. გაგზავნა: `set_message_sink` → `conn.send_json`
- [x] S2: ბრენჩი `main`-ზე rebase და push (`sandro/viewer-design`)
- [x] ლოგო, tray icon, შეტყობინებები „Connected to host“ / „Connection lost“ (`docs/DESIGN.md` 9)
- [x] აპის სახელი **Sarke** (viewer, tray, შეტყობინებები, DESIGN.md) + ლოგო „ს caret“ + `docs/brand/sarke.ico`, `tools/ui_preview_logo.exe`. exe-ების სახელები და `.rc` — ლუკა (`ARTEFACT/Sarke — სახელი და ლოგო`)
- [x] ეკრანების განლაგება (Displays, `docs/DESIGN.md` 10): `client_b/display_layout.*` + `display_settings.*`, `tools/sarke_displays.exe` — რეალური მონიტორები (live), მეორე PC drag-ით, Apply → `config.json` `client_side` (host/client), Identify; ტესტი `tools/display_layout_test.exe` (60). top/bottom — ლუკას `main.cpp`-ების შემდეგ (`ARTEFACT/ეკრანების განლაგება და Sarke-ის შეტყობინება`)
- [x] Sarke-ის შეტყობინება ზედა მარჯვნივ, NVIDIA-ს მსგავსად (`client_b/toast.*`, `docs/DESIGN.md` 9.3, `tools/ui_preview_toast.exe`); tray-ში „Display arrangement...“
- [x] ბრაუზერის პროტოტიპი `docs/preview/sarke-app.html` (+ `sarke-ui.html`): იგივე side-only მოდელი, ბრაუზერის რეალური ეკრანები, ახალი ბარათი; 95 ტესტი (`test_sarke_app.py`)
- [ ] ლუკა: tray + Displays + peer-ის დამახსოვრება `main.cpp`-ებში, top/bottom-ის დაშვება (იხ. იგივე ARTEFACT)
- [ ] დიზაინის შემოწმება DPI 125 / 150 / 200%-ზე და ორ მონიტორს შორის (`docs/DESIGN.md` 7)
- [x] M1b (ლუკა): ფანჯარა A ⇄ B `main.cpp`-ში — viewer-ის მხარე მზადაა (ჩართო თემომ, `host_a/window_link.*`)
- [x] ~~ფაზა 2: `host_a/remote_input.*`~~ → თემოს გადაეცა (გადაწყვეტილებები 2, #3; T8 `5fc0ab5`)

## რისკები
| რისკი | რა ვქნათ |
|---|---|
| გარემოს დაყენება (VS, vcpkg) დიდხანს გრძელდება | წინა დღით; ლუკა ამოწმებს, რომ 4-ივეზე build გადის |
| WGC capture ყველაზე რთულია | თემო იწყებს ყველაზე ადრე; ლუკა ეხმარება ფაზა 1-ის ბოლოს. Fallback: `PrintWindow`/GDI (ნელი, მაგრამ მუშაობს) |
| LL hook-ს Windows ჩუმად ხსნის, თუ callback ნელია | callback-ში მხოლოდ queue-ში ჩადება, არანაირი ლოგი/ქსელი |
| ფიზიკური A და B მხოლოდ ერთი წყვილია | ფაზა 1 მთლიანად ერთ კომპიუტერზე (`127.0.0.1`); A+B-ზე ტესტი M1-ზე და M2-ზე |
| Admin ფანჯრებში ინფუთი არ გადის | host ადმინით გაუშვი |
| ქსელის latency (hotspot) | Wi-Fi რაუტერი ან Ethernet |
