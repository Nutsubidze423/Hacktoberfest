პასუხი · სანდრო → ლუკა, თემო და მათი Claude · 17.10.2026

# S1–S3 მზადაა: viewer-ის ჩართვა ორივე მხარეს

Claude, ეს ფაილი ლუკას (და თემოს) სესიისთვისაა. პასუხია `გადაწყვეტილებები 2`-ზე და თემოს „ფანჯარა ორივე მხარეს“-ზე. ყველაფერი ბრენჩზეა `sandro/viewer-design`, **rebase გაკეთებულია `main`-ზე** (`5fc0ab5`-ზე), ორივე exe აიწყობა warning-ების გარეშე. PR-ს ელოდება.

## სტატუსი

| # | რა | სტატუსი |
| --- | --- | --- |
| S1 | receiver ლუკას არხზე | ✅ `VideoReceiver(FrameChannel&, ViewerWindow&)` — არხს გარედან იღებს, თავად არ უკავშირდება და reconnect-ს არ აკეთებს. `kvm::video_wire` წაშლილია |
| S2 | `main`-ში ატვირთვა | ✅ rebase + push. **ლუკა, merge შენზეა** — CMakeLists არ შემიცვლია |
| S3 | viewer-ის ინფუთი ორივე მხარეს | ✅ callback `std::function<bool(const proto::Message&)>`, viewer-ის დახურვა სტრიმისას → `StreamStop{.hwnd}` |
| — | თემოს კითხვა 2 (`remote_input`) | ✅ შენი გადაწყვეტილებით თემოსია (T8 უკვე `main`-შია) |

**შემოწმებულია:** `kvm_host --no-kvm --test-video` + `kvm_client` (control) → `viewer_test --connect 127.0.0.1`: receiver იღებს ფრეიმებს შენს არხზე (codec `test` არ ჩანს, MBPS-ში ითვლება), სუფთად ჩერდება. JPEG `fake_streamer`-ით: 1080p, 30 fps, ~9 ms. viewer-ის დახურვისას იგზავნება `{"type":"stream_stop","hwnd":…}`. დამალული viewer (`create(false)`) თავისით ჩნდება `stream_start`-ზე, ფოკუსის მოპარვის გარეშე.

## ლუკა: `main.cpp` (ორივე exe-ში ერთნაირად)

viewer-ს და tray-ს ერთ thread-ზე message loop სჭირდება (`GetMessage`/`DispatchMessage`). ქვემოთ ყველა `viewer.*` / `receiver.*` გამოძახება thread-safe-ია, გარდა `create`, `set_*_sink/handler/listener`-ისა (UI thread, ერთხელ).

```cpp
#include "tray_icon.h"
#include "video_receiver.h"
#include "viewer_window.h"

// UI thread, ერთხელ:
kvm::ViewerWindow viewer;
viewer.create(/*visible=*/false);                 // stream_start-ზე თავისით გამოჩნდება
viewer.set_jpeg_quality(cfg.jpeg_quality);
viewer.set_endpoint(peer_ip, cfg.video_port);     // მდგომარეობის ეკრანების ტექსტი
viewer.set_reconnects(true);                      // LOST-ზე „reconnecting“
viewer.set_message_sink([&](const kvm::proto::Message& m) { return conn.send_json(kvm::proto::encode(m)); });
viewer.set_close_handler([&] { ShowWindow(viewer.hwnd(), SW_HIDE); });  // stream_stop უკვე გაიგზავნა

// ყოველ ვიდეო კავშირზე (B: connect_video, A: VideoServer::accept):
auto video = /* std::unique_ptr<kvm::net::FrameChannel> */;
kvm::VideoReceiver receiver(*video, viewer);      // video-ს შემდეგ გამოაცხადე: destructor close()-ს
                                                  // და join-ს აკეთებს, მერე video შეიძლება განადგურდეს

// control კავშირის მდგომარეობა → viewer:
viewer.set_link_state(kvm::LinkState::Linked);    // ასევე Waiting / Connecting / Lost
// control შეტყობინებები viewer-ისთვის:
//   StreamStart s → viewer.start_stream(s);      StreamStop → viewer.stop_stream();
// სტატისტიკა (როცა conn.stats().rtt_us >= 0):
//   receiver.set_clock_offset_us(s.clock_offset_us);  viewer.set_rtt_ms(int(s.rtt_us / 1000));

// tray (სურვილისამებრ, ორივე exe-ში ხელმისაწვდომია — ორივე kvm_client_lib-ს ლინკავს):
kvm::TrayIcon tray;
tray.create(L"KVM", {.open = [&] { viewer.bring_to_front(); }, .quit = [&] { /* app exit */ }});
viewer.set_link_listener([&](kvm::LinkState before, kvm::LinkState now) {
    tray.on_link_change(before, now, endpoint_w, /*reconnects=*/true);
});
```

**viewer → owner შეტყობინებები** (`set_message_sink`-ით): `remote_mouse_move`, `mouse_button` (+ `x_norm`/`y_norm`), `mouse_wheel`, `key`, და `stream_stop {hwnd}` დახურვისას. KVM-ით ჩასმული მოვლენებიც (`kInjectedMarker`) ჩვეულებრივ იგზავნება, როგორც გადაწყვიტე.

## თემო

`x_norm`/`y_norm` = წერტილი **გაგზავნილ ფრეიმში** (0,0 = ზედა-მარცხენა პიქსელი, 1,1 = ქვედა-მარჯვენა), ზოლები და სათაურის ზოლი არ ითვლება. ფრეიმი = `share->monitor_rect()`, ამიტომ შენი გადათვლა ზუსტად ემთხვევა.

## ლუკას დარჩენილი (ჩემი მხრიდან თხოვნები)

1. **დიზაინის საერთო ფაილები:** `docs/DESIGN.md`-ის მიხედვით tokens/draw helpers შენია (`common/include/kvm/ui/`). ჯერ დროებით სანდროსთანაა: `client_b/ui_tokens_tmp.h`, `client_b/ui_draw_tmp.*` — გადატანისას იქიდან აიღე.
2. **ფონტი:** `assets/fonts/`-ში JetBrains Mono (Regular, Bold, OFL.txt). სანამ არ არის, viewer Consolas-ს იყენებს.
3. **აპის სახელი Windows-ის შეტყობინებაში:** ახლა ჩანს exe-ს სახელი, რადგან version resource არ არის. `.rc` (`FileDescription`) CMake-ში შენ უნდა დაამატო.
4. **„დაბრუნების“ ღილაკი სათაურის ზოლზე:** ჯერ არ არის — `docs/DESIGN.md`-ში არ წერია, დახურვა იგივეს აკეთებს. თუ გინდა, მითხარი: ჯერ DESIGN.md-ში დავამატებ, მერე კოდში.
5. viewer-ის მდგომარეობის ეკრანზე წერია „KVM CLIENT“. A-ზე viewer დამალულია, სანამ სტრიმი არ დაიწყება, ასე რომ იქ ეს ეკრანი პრაქტიკულად არ ჩანს. თუ A-ზეც გინდა ხილული, სხვა სათაური DESIGN.md-ში უნდა დაემატოს.

## ასევე ბრენჩზე

- Direct2D viewer: სათაურის ზოლი, მდგომარეობის ეკრანები, HUD (`docs/DESIGN.md` 4).
- ლოგო (C „Caret“), tray icon, შეტყობინებები „Connected to host“ / „Connection lost“ (`docs/DESIGN.md` 9).
- preview: `tools/ui_preview_viewer.exe` (ყველა მდგომარეობა სატესტო მონაცემებით; `--hidden` — A-ს ქცევა).
