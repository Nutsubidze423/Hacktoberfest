პასუხი · სანდრო → თემო, ლუკა და მათი Claude · 17.10.2026

# პასუხი „ფანჯარა ორივე მხარეს“-ზე და viewer-ის ჩართვა `kvm_client`-ში

Claude, ეს ფაილი შენთვისაა (ლუკას ან თემოს სესია). ყველაფერი, რაც ქვემოთ წერია, ბრენჩზეა `sandro/viewer-design` (ჯერ `main`-ში არ არის, PR-ს ელოდება).

## პასუხები თემოს კითხვებზე

**1. receiver `recv_frame`-ზე — გაკეთდა.** receiver ახლა ლუკას `connect_video` + `FrameChannel::recv_frame`-ს იყენებს (`7e2dc50`). დროებითი `kvm::video_wire` წაშლილია. `tools/fake_streamer` `VideoServer`-ით აგზავნის, ანუ ზუსტად იმას ლაპარაკობს, რასაც A-ს `video_sender`. ტესტი ლოკალურად: 1080p, 30 fps, ~9 ms capture → ეკრანი. **M1b სანდროს მხარეს აღარ არის დაბლოკილი.**

**2. `host_a/remote_input.*` — სანდრო თანახმაა, რომ თემომ აიღოს.** ლუკას პასუხს ველოდებით (კითხვა ორივეს ეხებოდა). CLAUDE.md-ის ცხრილი მხოლოდ ლუკას თანხმობის შემდეგ შეიცვალოს.

## viewer-ის ინფუთი — მზადაა (`67c0673`)

viewer აგზავნის ზუსტად `protocol.h`-ის შეტყობინებებს, `protocol.h` არ შეცვლილა:
- `remote_mouse_move {x_norm, y_norm}`, `mouse_button {button, down, x_norm, y_norm}`, `mouse_wheel {delta, horizontal}`, `key {vk, scan, down, extended}`.
- `x_norm`/`y_norm` ითვლება **დახატული გამოსახულების** მიმართ (0..1), აღებული ფრეიმის მთლიან ზომაზე. სათაურის ზოლი და aspect ratio-ს შავი ზოლები იგნორირდება.
- drag გამოსახულებიდან გასვლისას კიდეზე „ეწებება“ (clamp), `up` ყოველთვის მიდის.
- viewer-ის ფოკუსის დაკარგვისას ყველა დაჭერილ ღილაკს/კლავიშს `up` ეგზავნება — A-ზე არაფერი რჩება დაჭერილი.
- Alt+F4 ლოკალურია (viewer-ს ხურავს), A-ზე არ მიდის.

**თემო, შეამოწმე:** `x_norm`/`y_norm` = წერტილი **გაგზავნილ ფრეიმში** (0,0 = ფრეიმის ზედა-მარცხენა პიქსელი). თუ ფრეიმი = `share->monitor_rect()` მთლიანად, მაშინ შენი გადათვლა `monitor_rect`-ზე ზუსტად ემთხვევა.

## ლუკა: ჩართვა `client_b/main.cpp`-ში

`main.cpp`-ში `TODO(luka): hand frames to the viewer (sandro)` — `receive_video`-ის ნაცვლად `VideoReceiver` (მას საკუთარი reconnect აქვს და LatestSlot-ით მხოლოდ ბოლო ფრეიმს აჩვენებს):

```cpp
#include "tray_icon.h"
#include "video_receiver.h"
#include "viewer_window.h"

// UI thread-ზე (მას message loop სჭირდება — GetMessage/DispatchMessage):
kvm::ViewerWindow viewer;
viewer.create();
viewer.set_jpeg_quality(cfg.jpeg_quality);
viewer.set_input_sink([&](const kvm::proto::Message& m) { conn.send_json(kvm::proto::encode(m)); });

const std::wstring endpoint = /* host:port */;
kvm::TrayIcon tray;
tray.create(L"KVM client", {.open = [&] { viewer.bring_to_front(); },
                            .quit = [&] { PostMessageW(viewer.hwnd(), WM_CLOSE, 0, 0); }});
viewer.set_link_listener([&](kvm::LinkState before, kvm::LinkState now) {
    tray.on_link_change(before, now, endpoint, /*reconnects=*/true);
});

kvm::VideoReceiver receiver(cfg.host_ip, cfg.video_port, viewer);

// control არხიდან (ნებისმიერი thread-იდან, thread-safe):
const auto s = conn.stats();
if (s.rtt_us >= 0) {                                            // -1 = ჯერ არ გაზომილა
    receiver.set_clock_offset_us(s.clock_offset_us);           // HUD-ის LAT; მანამდე „—“
    viewer.set_rtt_ms(static_cast<int>(s.rtt_us / 1000));     // HUD-ის RTT; მანამდე „—“
}
viewer.start_stream(title, width, height);                     // stream_start-ზე
viewer.stop_stream();                                          // stream_stop-ზე
```

## ლუკას გადასაწყვეტი / გასაკეთებელი

1. **UI thread:** viewer-ს და tray-ს ერთ thread-ზე message loop სჭირდება. `main.cpp`-ის session loop მეორე thread-ზე გადავიდეს, ან UI ცალკე thread-ზე.
2. **დიზაინის საერთო ფაილები:** `docs/DESIGN.md`-ის მიხედვით tokens/draw helpers შენია (`common/include/kvm/ui/`). ჯერ დროებით სანდროსთანაა: `client_b/ui_tokens_tmp.h`, `client_b/ui_draw_tmp.*`. გადატანისას იქიდან აიღე.
3. **ფონტი:** `assets/fonts/`-ში JetBrains Mono (Regular, Bold, OFL.txt). სანამ არ არის, viewer Consolas-ს იყენებს.
4. **Windows-ის შეტყობინებაში აპის სახელი:** ახლა წერია exe-ს სახელი (მაგ. `viewer_test.exe`), რადგან exe-ს version resource არ აქვს. `.rc` ფაილი (`FileDescription` = „KVM client“) CMake-ში შენ უნდა დაამატო.
5. **tray `kvm_host`-ზეც:** `TrayIcon`/`app_logo` ახლა `client_b`-შია. თუ A-ზეც გინდა („B დაუკავშირდა“), ორივე `common`-ში გადაიტანე.

## დემეტრეს გასათვალისწინებლად

როცა A-ს კლავიატურა KVM-ით B-ზეა და viewer-ს ფოკუსი აქვს, A-ს კლავიშები viewer-ის გავლით A-ზე დაბრუნდება. საჭიროა წესი (დემეტრეს ფაზა 2: „viewer-ზე ფოკუსისას KVM-ის ქცევა“). მაგალითად: viewer-ის ფოკუსისას `input_inject` კლავიშებს არ ემულირებს, ან viewer KVM-იდან მოსულ ინფუთს არ აგზავნის.

## სანდროს ბრენჩზე სხვა ახალი

- Direct2D viewer: სათაურის ზოლი, მდგომარეობის ეკრანები, HUD (`docs/DESIGN.md`, ნაწილი 4).
- ლოგო (კონცეფცია C, „Caret“), tray icon „show hidden icons“-ში, შეტყობინებები „Connected to host“ / „Connection lost“ (`docs/DESIGN.md`, ნაწილი 9).
- preview: `tools/ui_preview_viewer.exe` (ყველა მდგომარეობა სატესტო მონაცემებით).
