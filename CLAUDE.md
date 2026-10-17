# CLAUDE.md — KVM + Window Streaming დემო (C++)

## კომუნიკაცია
- მომხმარებელთან **ქართულად** ისაუბრე. კოდი, კომენტარები, ცვლადების სახელები და commit-ები: **ინგლისურად**.
- მომხმარებლები AI-ს დახმარებით სწავლობენ და აწყობენ პროექტს. ცვლილების შემდეგ მოკლედ ახსენი, რა გააკეთე და როგორ გაუშვან/შეამოწმონ.
- ერთდროულად ერთ ამოცანაზე იმუშავე.

## გუნდი — წაიკითხე პირველ რიგში
პროექტზე მუშაობს 4 დეველოპერი, თითოეული საკუთარ კომპიუტერზე, საკუთარ Claude-ის სესიაში (vibe coding). სრული დეტალები (ვადები, ამოცანები, ჩეკლისტები) — [docs/PLAN.md](docs/PLAN.md).

| სახელი | ტრეკი | ფაილები | პირველი ამოცანა |
|---|---|---|---|
| **ლუკა** (lead) | Core / ქსელი / ინტეგრაცია; PR review და merge | root CMake, `CMakePresets.json`, `vcpkg.json`, `common/` (protocol, net, config, log, queue), `host_a/main.cpp`, `client_b/main.cpp`, `tools/ping_test.cpp` | M0: repo-ს ჩონჩხი (CMake + vcpkg + ცარიელი exe-ები) და `protocol.h` |
| **დემეტრე** | KVM ინფუთი: hooks, კიდიდან გადასვლა, suppress, გადაუდებელი ღილაკი, ემულაცია B-ზე | `common/win32_input.*`, `common/dpi.*`, `host_a/input_capture.*`, `client_b/input_inject.*` | `win32_input` (SendInput wrappers) — სანდროსაც სჭირდება |
| **თემო** | ვიდეო A-ზე: ფანჯრის არჩევა, WGC capture, JPEG, გაგზავნა | `host_a/window_picker.*`, `capture.*`, `jpeg_encoder.*`, `video_sender.*`, `tools/capture_test.cpp` | `window_picker`, მერე capture ფაილში შენახვით |
| **სანდრო** | Viewer B-ზე + უკუ-ინფუთი A-ს ფანჯარაში | `client_b/viewer_window.*`, `jpeg_decoder.*`, `video_receiver.*`, `host_a/remote_input.*`, `tools/fake_streamer.cpp` | JPEG decode + viewer ფანჯარა ფაილიდან წაკითხული სურათით |

### როცა მომხმარებელი წერს „მე ვარ <სახელი>“
1. **წაიკითხე** `docs/PLAN.md` — მისი როლი, ფაილები, ფაზის ამოცანები და ჩეკლისტი („პროგრესი“ სექციაში).
2. **შეამოწმე** `git status`, `git log --oneline -15`, მიმდინარე ბრენჩი და შესაბამისი ფაილები — რა არის რეალურად გაკეთებული (ჩეკლისტს ბრმად ნუ ენდობი).
3. **აუხსენი** მოკლედ, ქართულად:
   - ვინ არის გუნდში და რა არის მისი როლი (1–2 წინადადება);
   - რა არის უკვე გაკეთებული მის ტრეკზე და დანარჩენ გუნდში, რაც მას ეხება (მაგ. დამოკიდებულება სხვის ფაილზე);
   - რომელია შემდეგი ამოცანა, როგორ გავაკეთებთ და როგორ შევამოწმებთ, რომ მუშაობს.
4. **დაიწყე მუშაობა** იმავე პასუხში — ახალი ბრენჩი (`<name>/<task>`) და პირველი ნაბიჯი. ნებართვას ნუ ელოდები, თუ რამე ნამდვილად გაუგებარი არ არის.
5. თუ ამოცანა სხვის ჯერ დაუმთავრებელ კოდზეა დამოკიდებული — დროებითი stub/mock გააკეთე **საკუთარ ფაილებში** და უთხარი, ვის ელოდება.

გარემოს შემოწმება პირველ სესიაზე: თუ `cmake`, `cl`, `ninja` ან `VCPKG_ROOT` არ არსებობს — ჯერ დაეხმარე Visual Studio 2022 („Desktop development with C++“) და vcpkg-ის დაყენებაში.

წესები გუნდურ მუშაობაზე:
- შეცვალე **მხოლოდ იმ დეველოპერის ფაილები**, ვისთანაც მუშაობ. სხვისი ფაილის შეცვლა თუ საჭიროა — ნუ შეცვლი; უთხარი, ვის უნდა მიმართოს.
- `common/include/kvm/protocol.h` იცვლება მხოლოდ ლუკასთან სესიაში.
- თუ დეველოპერის სახელი არ იცი — ჰკითხე, სანამ კოდს შეცვლი.
- ამოცანის დასრულებისას: build გადის → ტესტი გავლილია → მონიშნე პუნქტი (`[x]`) `docs/PLAN.md`-ში იმავე commit-ში.
- ბრენჩი ამოცანაზე: `<სახელი ლათინურად>/<ამოცანა>`, მაგ. `temo/wgc-capture`. main-ში პირდაპირ commit არ გააკეთო.

## პროექტის მიზანი
ორი Windows კომპიუტერი ერთ ლოკალურ ქსელში:
- **A** — სახლის PC, 2 მონიტორი. ფლობს ფიზიკურ მაუსს და კლავიატურას. ეშვება **host** როლში.
- **B** — ლეპტოპი, დგას A-ს გვერდით. ეშვება **client** როლში.

საბოლოო მიზანი: A-ს მაუსი/კლავიატურა გადადის B-ს ეკრანზე (როგორც Deskflow / Mouse Without Borders), და A-ს ცალკეული ფანჯარა (მაგ. Photoshop) ტრანსლირდება B-ზე ისე, თითქოს B იყოს A-ს მესამე მონიტორი. ფანჯრის პროცესი ყოველთვის A-ზე მუშაობს; B მხოლოდ აჩვენებს სურათს და აბრუნებს ინფუთს.

## დემოს ფარგლები (≈10 საათი, 4 ადამიანი)
**შედის:**
1. მაუსი/კლავიატურა A → B ეკრანის კიდიდან გადასვლით (B-ს კონტროლი).
2. A-ზე არჩეული ერთი ფანჯრის სტრიმი B-ზე ცალკე ფანჯარაში.
3. სტრიმულ ფანჯარაზე კლიკები/სქროლი/კლავიატურა → A-ს ორიგინალ ფანჯარას.

**არ შედის (მერე):** ვირტუალური მონიტორის დრაივერი (IddCx), ფანჯრის drag-ით გადატანა, დაშიფვრა/დაწყვილება, clipboard, ფაილები, ინსტალერი, პენის წნევა.

## ტექნოლოგიები
- **C++20**, MSVC (Visual Studio 2022), **CMake ≥ 3.25 + Ninja**, **vcpkg** (manifest mode). მხოლოდ Windows 10/11 x64.
- vcpkg: `nlohmann-json` (JSON), `libjpeg-turbo` (JPEG). C++/WinRT და D3D11 — Windows SDK-დან (≥ 10.0.19041).
- ქსელი: Winsock2, ბლოკირებადი სოკეტები + ცალკე thread-ები.
- A hooks: `SetWindowsHookEx` (`WH_MOUSE_LL`, `WH_KEYBOARD_LL`) + Raw Input relative მოძრაობისთვის.
- ინფუთის ემულაცია: `SendInput`, `SetCursorPos`, `SetForegroundWindow`.
- Capture: Windows.Graphics.Capture (C++/WinRT) → D3D11 → BGRA. Fallback: `PrintWindow`.
- კომპრესია: v1 **JPEG** (TurboJPEG). v2: Media Foundation H.264 hardware MFT.
- B-ზე ჩვენება: v1 Win32 + `StretchDIBits`; v2 D3D11 swapchain.
- Thread-ებს შორის: `kvm::BlockingQueue<T>`, ვიდეოსთვის `kvm::LatestSlot<T>` (მხოლოდ ბოლო ფრეიმი).

A-ს ვიდეოკარტა: _TODO — შეავსე (NVIDIA / AMD / Intel)_. B: Intel UHD 620.

## სტრუქტურა
```
CMakeLists.txt             # root  (ლუკა)
CMakePresets.json          # x64-debug / x64-release  (ლუკა)
vcpkg.json                 # (ლუკა)
config.json                # IP, პორტები, B-ს მდებარეობა (left/right), ხარისხი
docs/PLAN.md               # როლები, ვადები, ჩეკლისტები
common/                    # static lib kvm_common
  include/kvm/
    protocol.h             # შეტყობინებები (ლუკა) — გაყინული კონტრაქტი
    net.h  config.h  log.h  queue.h            # (ლუკა)
    win32_input.h  dpi.h                       # (დემეტრე)
  src/*.cpp
host_a/                    # kvm_host.exe
  main.cpp                 # (ლუკა)
  input_capture.*          # hooks + edge detection + suppress (დემეტრე)
  window_picker.*  capture.*  jpeg_encoder.*  video_sender.*   # (თემო)
  remote_input.*           # B-დან მოსული ინფუთი → A-ს ფანჯარა (სანდრო)
client_b/                  # kvm_client.exe
  main.cpp                 # (ლუკა)
  input_inject.*           # A-დან მოსული ინფუთის ემულაცია (დემეტრე)
  viewer_window.*  jpeg_decoder.*  video_receiver.*            # (სანდრო)
tools/
  ping_test.cpp            # (ლუკა)
  capture_test.cpp         # ფანჯარა → .jpg (თემო)
  fake_streamer.cpp        # .jpg-ები → viewer (სანდრო)
```

## ქსელური პროტოკოლი
- A არის სერვერი, B უკავშირდება.
- პორტი **50500** — control + input: `[u32 len][JSON]`, big-endian.
- პორტი **50501** — ვიდეო: `[u32 header_len][u32 payload_len][header JSON][payload]`, big-endian.
- `TCP_NODELAY` ორივე სოკეტზე.

შეტყობინებები (`type` ველით):
| type | მიმართულება | ველები |
|---|---|---|
| `hello` | B→A | `protocol_version`, `screen_w`, `screen_h`, `dpi_scale`, `hostname` |
| `focus_enter` / `focus_leave` | A→B | `y_norm` (კურსორის შესვლის წერტილი) |
| `mouse_move` | A→B | `dx`, `dy` (relative) |
| `mouse_button` | ორივე | `button` (`left`/`right`/`middle`/`x1`/`x2`), `down` (+ B→A-ზე `x_norm`, `y_norm`) |
| `mouse_wheel` | ორივე | `delta`, `horizontal` |
| `key` | ორივე | `vk`, `scan`, `down`, `extended` |
| `stream_start` | A→B | `hwnd`, `title`, `width`, `height` |
| `stream_stop` | A→B | `hwnd` |
| `remote_mouse_move` | B→A | `x_norm`, `y_norm` (სტრიმულ ფანჯარაში, 0..1) |
| `ping` / `pong` | ორივე | `t` (+ `pong`-ში `t_peer` — clock offset-ისთვის) |
| `frame` (50501 header) | A→B | `seq`, `t_capture_us`, `width`, `height`, `codec` |

## მნიშვნელოვანი წესები
- **DPI:** ორივე პროცესი იწყება `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`-ით (ან manifest-ით). ყოველთვის ფიზიკურ პიქსელებში იმუშავე.
- **კოორდინატები სტრიმულ ფანჯარაზე:** B აგზავნის ნორმალიზებულ (0..1) კოორდინატებს. A გადაითვლის ფანჯრის **client area**-ს მიმდინარე rect-ით (ფანჯარა შეიძლება გადაადგილდეს).
- **Hook-ები:** როცა კონტროლი B-ზეა, A-ს ლოკალური ინფუთი იბლოკება (hook აბრუნებს 1), relative მოძრაობა მოდის Raw Input-იდან და იგზავნება B-ზე.
- **გადაუდებელი გამოსვლა:** `Ctrl+Alt+Shift+Q` ყოველთვის აბრუნებს კონტროლს A-ზე და აჩერებს hook-ებს. ეს არასოდეს წაშალო და არასოდეს დაბლოკო.
- Hook callback-ებში მძიმე სამუშაო არ გააკეთო (არც ლოგი, არც ქსელი) — მხოლოდ queue-ში ჩადება. ნელ hook-ს Windows ჩუმად ხსნის.
- ვიდეო: თუ ქსელი ვერ ასწრებს, ძველი ფრეიმები გადააგდე (ყოველთვის ბოლო ფრეიმი), queue არ დააგროვო.
- ელევირებული (Admin) ფანჯრებში ინფუთი ჩვეულებრივი პროცესიდან არ გადის — თუ საჭიროა, host ადმინით გაუშვი.
- RAII Win32/COM რესურსებისთვის (`winrt::com_ptr`, `wil`-ის მსგავსი wrapper-ები ან საკუთარი `unique_handle`). raw `new`/`delete` არა.

## Build და გაშვება
გარემოს დაყენება (ერთხელ): `powershell -ExecutionPolicy Bypass -File tools\setup_env.ps1` (VS 2022 + C++ + CMake + Ninja + vcpkg). Firewall-ისთვის ადმინით: იგივე `-Firewall`-ით.

ახალი `.cpp` ფაილის დამატებისას CMakeLists-ის შეცვლა **არ არის საჭირო** — `common/src/`, `host_a/`, `client_b/` ფაილები ავტომატურად ერთვება (glob); თითოეული `tools/<name>.cpp` ხდება ცალკე `<name>.exe`. CMakeLists-ს ფლობს ლუკა.

„Developer PowerShell for VS 2022“-ში:
```
cmake --preset x64-release
cmake --build --preset x64-release
# A-ზე:
build\x64-release\host_a\kvm_host.exe
# B-ზე:
build\x64-release\client_b\kvm_client.exe --host <A-ს IP>
```
ლოკალური ტესტი ერთ კომპიუტერზე: `--host 127.0.0.1`.
Windows Firewall-ში დაუშვი `kvm_host.exe` პორტებზე 50500–50501 (Private network).

## ჩეკლისტი (საერთო milestone-ები; დეტალური — docs/PLAN.md)
- [ ] M0. ყველას აქვს VS 2022 + vcpkg; repo-ს ჩონჩხი აიწყობა 4-ივეზე; `protocol.h` გაყინულია
- [ ] ფაზა 1. ყველა ტრეკი დამოუკიდებლად მუშაობს (net, hooks/inject, capture/encode, viewer)
- [ ] M1. ფიზიკურ A+B-ზე: KVM A → B და ფანჯრის JPEG სტრიმი B-ზე
- [ ] ფაზა 2. viewer → A: მაუსი/კლიკები/სქროლი/კლავიატურა/ფოკუსი; სქროლი და extended კლავიშები KVM-ში
- [ ] M2. ტესტი Photoshop-ით; latency/ხარისხის მორგება; დემოს სცენარი

## შემდეგი ეტაპები (დემოს მერე)
H.264 (Media Foundation / NVENC) → D3D11 zero-copy pipeline → IddCx ვირტუალური მონიტორი A-ზე B-ს ზომით → ფანჯრის drag-ით გადატანა → B-ზე ფანჯრების overlay იმავე პოზიციაზე → TLS + PIN დაწყვილება → clipboard.
