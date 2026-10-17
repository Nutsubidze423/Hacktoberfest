ცნობა · სანდრო → ლუკა (და თემო, დემეტრე — ინფორმაციისთვის) და მათი Claude · 18.10.2026

# ეკრანების განლაგება (Displays) და Sarke-ის საკუთარი შეტყობინება: რა გაკეთდა და რა რჩება შენზე

Claude, ეს ფაილი ლუკას სესიისთვისაა. სანდრომ ითხოვა: (1) „მეორე PC სად დგას“ — ნამდვილად მომუშავე პარამეტრი, რომელიც რეალურ მონიტორებს ხედავს და არა მხოლოდ დიზაინი; (2) შეტყობინება NVIDIA-ს მსგავსად ეკრანის **ზედა მარჯვენა** კუთხეში, საკუთარი დიზაინით, Windows-ის ჩვეულებრივი toast-ის ნაცვლად. ორივე სანდროს ფაილებშია; `main.cpp`-ებში ჩართვა შენია.

## რა გაკეთდა (ბრენჩი `sandro/displays`, `sandro/sarke-brand`-ის თავზე)

- **`client_b/display_layout.*`** — ლოგიკა UI-ს გარეშე: ამ PC-ის რეალური მონიტორები (`display::monitors()` VDD-ის გარეშე), გადაკვეთის კიდე (იგივე წესი, რაც დემეტრეს `OuterEdge`-ში), მეორე PC-ის ადგილი, host/client → `client_side`, `config.json`-ის ჩაწერა (დანარჩენი გასაღებები და მათი თანმიმდევრობა რჩება; გაუმართავ ფაილს არასოდეს გადააწერს; temp + swap), `peer.json` (`%LOCALAPPDATA%\Sarke\`), როლის გამოცნობა `host_ip`-დან. ტესტი: `tools/display_layout_test.exe` (60 შემოწმება).
- **`client_b/display_settings.*`** — ფანჯარა „Where is the other PC?“ (`docs/DESIGN.md` 10): რეალური მონიტორები (live: `WM_DISPLAYCHANGE`, DPI, ყოველ 2 წმ), მეორე PC drag-ით / ღილაკებით / ისრებით, ნარინჯისფერი კიდე = სადაც კურსორი რეალურად გადადის, Apply → `config.json`, Identify (ნომერი ყველა ეკრანზე), „Windows display settings“.
- **`tools/sarke_displays.exe`** — იგივე ფანჯარა ცალკე (window app, კონსოლის გარეშე): `--role host|client`, `--config <path>`, `--peer 1920x1200@1.5 <name>`.
- **`client_b/toast.*`** — Sarke-ის შეტყობინების ბარათი (`docs/DESIGN.md` 9.3): ზედა მარჯვნივ, slide + fade (Windows-ის ანიმაციები გამორთულია → მხოლოდ fade), დროის ხაზი, hover აჩერებს, click = open, × = დახურვა. `TrayIcon::notify` ახლა მას იყენებს (Windows-ის toast მხოლოდ fallback-ია). Preview: `tools/ui_preview_toast.exe --sequence`.
- **tray მენიუ:** ახალი პუნქტი „Display arrangement...“ (`TrayIcon::Actions::displays`, ჩანს მხოლოდ მაშინ, როცა მიცემულია).
- ბრაუზერის პროტოტიპი `docs/preview/sarke-app.html` იგივე მოდელზეა (side-only, ბრაუზერის `getScreenDetails()`-ით რეალური ეკრანები) — 95 Playwright ტესტი: `python docs/preview/test_sarke_app.py`.

## რატომ მხოლოდ მხარე (side) და არა „offset / alignment“

KVM კურსორს **პროპორციულად** ასახავს: ამ PC-ის მთელი გარე კიდე იმ მხარეს (`OuterEdge`, t = 0..1) → მეორე PC-ის მთელ კიდეზე. ამიტომ ერთადერთი რეალური პარამეტრია `client_side`. offset-ს backend-ში აზრი არ აქვს — პროტოტიპიდან ამოვიღე.

## ლუკა — შენი ფაილები

1. **top / bottom ჩართვა.** KVM-ის კოდი (დემეტრეს `OuterEdge`, `input_capture`, `input_inject`) და VDD-ის განთავსება (თემოს `virtual_display`) ოთხივე მხარეს უკვე უჭერს მხარს, მაგრამ:
   - `host_a/main.cpp` (~309): `client_side` უნდა იყოს `left`/`right`, თორემ KVM ითიშება;
   - `client_b/main.cpp` (~220): იგივე, მაგრამ `return 1` — client საერთოდ არ ეშვება.
   შემოთავაზება: ორივეგან დატოვე მხოლოდ `if (!side)` (უცნობი სიტყვა). შემდეგ `client_b/display_layout.h`-ში `kVerticalSidesAccepted = true` (ერთი ხაზი — მე ან შენ), და Displays-ში TOP/BOTTOM ჩაირთვება. მანამდე ფანჯარა მათ არ გთავაზობს, რომ client არ გაჩერდეს.
2. **Hello-ზე peer-ის დამახსოვრება** (ორივე `main.cpp`, სადაც `proto::Hello` მოდის):
   ```cpp
   #include "display_layout.h"
   kvm::layout::remember_peer({.hostname = h->hostname, .width = h->screen_w, .height = h->screen_h, .scale = h->dpi_scale});
   ```
   (ან, თუ ფანჯარა გახსნილია, `displays.set_peer(...)` — ის თავადაც იმახსოვრებს). ამის მერე Displays მეორე PC-ს მის რეალურ ზომაში აჩვენებს, offline-შიც.
3. **tray + Displays + ბარათი რეალურ აპებში.** `TrayIcon` ჯერ მხოლოდ ჩემს tools-შია (`viewer_test`, `ui_preview_viewer`). `main.cpp`-ებში ქსელის ციკლი მთავარ thread-ზეა, tray-ს კი message loop სჭირდება — მაგ. ცალკე UI thread:
   ```cpp
   std::jthread ui([&](std::stop_token st) {
       kvm::DisplaySettings displays;
       kvm::TrayIcon tray;
       tray.create(kvm::ui::kAppName, {.open = [&] { /* viewer.bring_to_front() */ },
                                       .displays = [&] { displays.show({.role = kvm::layout::Role::Host}); },  // client-ზე Client
                                       .quit = [&] { g_running = false; }});
       // link state -> tray.on_link_change(before, now, endpoint, reconnects) (ამ thread-ზე, მაგ. PostThreadMessage-ით)
       MSG m; while (!st.stop_requested() && GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
   });
   ```
   `DisplaySettings`, `TrayIcon` და `Toast` — UI thread only; `DisplaySettings::set_peer` ნებისმიერი thread-იდან შეიძლება.
4. **(სურვილისამებრ, protocol.h)** `hello`-ში `client_side` ველი: B აიღებს A-ს მნიშვნელობას და ორივე PC-ზე ცალ-ცალკე დაყენება აღარ იქნება საჭირო. დღეს ფანჯარა სტატუსის ხაზში ეუბნება მომხმარებელს, მეორე PC-ზე რა უნდა დააყენოს.

## როგორ შევამოწმოთ

```
build\x64-release\tools\display_layout_test.exe        # 60 შემოწმება, exit 0
build\x64-release\tools\sarke_displays.exe              # ფანჯარა; Apply წერს config.json-ში
build\x64-release\tools\ui_preview_toast.exe --sequence # ბარათები ზედა მარჯვნივ
```
