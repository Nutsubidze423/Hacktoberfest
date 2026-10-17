კონტექსტი · თემო → ლუკა და მისი Claude · 17.10.2026

# B როგორც მესამე მონიტორი: დრაივერი და დისპლეი

Claude, ეს ფაილი შენთვისაა. ტესტი 1 გავიდა: ლუკასა და დემეტრეს კომპიუტერები TCP-ით ერთმანეთს დაუკავშირდნენ. ამასობაში თემომ თავის ლეპტოპზე ვირტუალური მონიტორის იდეა შეამოწმა. აქ წერია, რა გააკეთა, რა გაირკვა და როგორ გაიმეორო იგივე ლუკას კომპიუტერზე (HOST, A). ბოლოს არის 4 კითხვა, რომლებზეც ლუკამ უნდა გადაწყვიტოს.

## იდეა მოკლედ

ცალკეული ფანჯრის JPEG-სტრიმის ნაცვლად A-ზე ვქმნით ვირტუალურ მონიტორს (მზა, ხელმოწერილი IddCx დრაივერით) და მის მთელ სურათს ვაგზავნით B-ზე. ფანჯარას გადაათრევ ამ მონიტორზე: A-ს ეკრანებიდან ის ქრება, B-ზე კი ჩანს და იქიდან იმართება. იგივე, ცხრილებით: https://claude.ai/artifact/RUAXjASyRGJx89fqbjCHxz (ლუკას წვდომას თემო აძლევს).

რატომ არ გამოდგება ფანჯრის ჩაკეცვა ან ეკრანს მიღმა გატანა:
- ჩაკეცილ ფანჯარას DWM არ ხატავს, ამიტომ WGC-ს გადასაღები არაფერი აქვს.
- `SendInput` ვერ მისწვდება ფანჯარას, რომელიც ეკრანზე არ არის. `PostMessage` Photoshop-ზე არ მუშაობს.
- ფანჯრის WGC capture-ში მენიუები და დიალოგები არ ჩანს, რადგან ისინი ცალკე ფანჯრებია. მონიტორის capture-ში ჩანს.

## რა გაკეთდა თემოს ლეპტოპზე

| | შედეგი |
| --- | --- |
| დრაივერი | Virtual Display Driver 25.7.23 (MIT), hardware ID `Root\MttVDD`. Device Manager-ში: Display adapters → „Virtual Display Driver“, მონიტორი: „Generic Monitor (VDD by MTT)“ |
| ხელმოწერა | დრაივერი და Control აპი: SignPath Foundation; `devcon.exe`: Microsoft. test mode და Secure Boot-ის გამორთვა არ დასჭირდა |
| დაყენება | `VDD Control.exe` → Install (GUI-დან, თემომ თავად) |
| პირველი პრობლემა | დაყენების მერე მონიტორი დაკავშირებული იყო, მაგრამ დესკტოპზე არ ჩანდა: Windows „PC screen only“ რეჟიმში იყო. `DisplaySwitch.exe /extend`-მა გაასწორა |
| შედეგი | `\\.\DISPLAY5`: 1920×1080 @ 60 Hz, პოზიცია (1920, 0), ლეპტოპის 1920×1200 ეკრანის მარჯვნივ (Intel Iris Xe) |
| პოზიცია კოდიდან | `ChangeDisplaySettingsExW` + `DM_POSITION`: გადავიდა (−1920, 0)-ზე და უკან. ორივეჯერ `DISP_CHANGE_SUCCESSFUL` |
| ჯერ არ შემოწმებულა | რეზოლუციის შეცვლა კოდიდან, WGC capture ვირტუალურ მონიტორზე, A-ზე ორი ფიზიკური მონიტორით |

## როგორ გაიმეორო ლუკას კომპიუტერზე (A)

**დრაივერს ლუკა თავად აყენებს.** თემოს სესიაში Claude Code-ის auto mode-მა Claude-ს ადმინით დრაივერის დაყენება დაბლოკა. ეს სწორია: ცვლილება სისტემაში მუდმივად რჩება. შენ ჩამოტვირთე და შეამოწმე ფაილი, ლუკას კი ზუსტი ნაბიჯები მიეცი.

1. **ჩამოტვირთვა და შემოწმება** (შენ აკეთებ, 68 MB, ცალკე ცარიელ საქაღალდეში):
   ```powershell
   $dir = "$env:TEMP\vdd"; New-Item -ItemType Directory -Force $dir | Out-Null
   $zip = "$dir\VDD.Control.25.7.23.zip"
   Invoke-WebRequest https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VDD.Control.25.7.23.zip -OutFile $zip -UseBasicParsing
   (Get-FileHash $zip).Hash -eq 'A701F2272E9FCF382849B24F913C6DD07597B3B1116525F2E90182F019609154'   # უნდა იყოს True
   Expand-Archive $zip "$dir\x"
   ```
   ჰეში ემთხვევა winget-ის მანიფესტს (`VirtualDrivers.Virtual-Display-Driver`). `SignedDrivers\x86\VDD`-ში რეალურად x64 (`NTamd64`) პაკეტია.
2. **დაყენება** (ლუკა აკეთებს): `$env:TEMP\vdd\x\VDD Control.exe` → **Install** → UAC-ზე **Yes** → Windows Security-ზე **Install**. თუ აუდიო დრაივერსაც (Virtual Audio Driver) შემოგთავაზებს, გამოტოვე.
   ალტერნატივა ადმინის PowerShell-ში, მხოლოდ display დრაივერი (თემოსთან ეს გზა არ გაშვებულა):
   ```powershell
   $x = "$env:TEMP\vdd\x"
   New-Item -ItemType Directory -Force C:\VirtualDisplayDriver | Out-Null
   Copy-Item "$x\SignedDrivers\x86\VDD\vdd_settings.xml" C:\VirtualDisplayDriver\
   & "$x\Dependencies\devcon.exe" install "$x\SignedDrivers\x86\VDD\MttVDD.inf" Root\MttVDD
   ```
3. **შემოწმება:**
   ```powershell
   Get-PnpDevice -Class Display, Monitor | Where-Object FriendlyName -match 'Virtual Display|VDD' | Format-Table FriendlyName, Status
   DisplaySwitch.exe /extend   # თუ მონიტორი OK-ა, მაგრამ დესკტოპზე არ ჩანს
   Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.Screen]::AllScreens | Format-Table DeviceName, Bounds, Primary
   ```
   ორი ფიზიკური მონიტორის მქონე A-ზე ნომერი `DISPLAY5` არ იქნება, ამიტომ კოდში ნუ ჩაწერ. `Screen.Bounds` PowerShell-ში შეიძლება DPI-ით იყოს გადათვლილი. ზუსტ ფიზიკურ პიქსელებს `EnumDisplaySettingsW` იძლევა.
4. **ლუკას გააფრთხილე:** სანამ viewer არ გვაქვს, ვირტუალური მონიტორი არსად ჩანს. მაუსი იმ კიდეზე „გაქრება“. ფანჯარას უკან **Win+Shift+←** აბრუნებს. **Win+P → PC screen only** მონიტორს დროებით თიშავს.

## კოდში (`host_a/virtual_display.*`, თემო დაწერს)

- **პოვნა:** `EnumDisplayDevicesW` ციკლში ის adapter, რომლის `DeviceString == L"Virtual Display Driver"` და `StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP`.
- **პოზიცია** (ფიზიკურ პიქსელებში):
  ```cpp
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  dm.dmFields = DM_POSITION;
  dm.dmPosition = {x, y};
  ChangeDisplaySettingsExW(device, &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);  // e.g. L"\\\\.\\DISPLAY5"
  ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);  // apply; both return DISP_CHANGE_SUCCESSFUL
  ```
  მხარე: right → `x` = დანარჩენი მონიტორების ყველაზე მარჯვენა კიდე; left → `x` = ყველაზე მარცხენა კიდე − სიგანე; top/bottom ანალოგიურად `y`-ით. primary ყოველთვის (0, 0)-ზე რჩება.
- **რეზოლუცია:** B უკვე აგზავნის `hello`-ში `screen_w`/`screen_h`-ს. თუ ეს რეჟიმი `C:\VirtualDisplayDriver\vdd_settings.xml`-ში არ არის (ნაგულისხმევად: 800×600, 1366×768, 1920×1080, 2560×1440, 3840×2160), იქ `<resolution>` უნდა დაემატოს და დრაივერი გადაიტვირთოს (Device Manager → disable/enable). მერე რეჟიმი იცვლება `DM_PELSWIDTH | DM_PELSHEIGHT`-ით. **ჯერ არ შემოწმებულა.**
- **capture:** WGC `IGraphicsCaptureItemInterop::CreateForMonitor`. ფანჯრის capture-ისგან მხოლოდ item-ის შექმნით განსხვავდება. თემოს შემდეგი ამოცანა, `capture.*`, ორივე ვარიანტს დაუჭერს მხარს, ამიტომ შენს გადაწყვეტილებას არ ელოდება.

## როგორ ერთდება არსებულ კავშირთან

`net.*`, `hello` და `ping`/`pong` არ იცვლება.

1. B → `hello` (`screen_w`, `screen_h`): უკვე მუშაობს.
2. A ვირტუალურ მონიტორს B-ს ზომაზე აყენებს და config-ის მხარეს სვამს (შეიძლება არსებული `client_side` გამოვიყენოთ).
3. A → `stream_start`. შესაძლოა `protocol.h`-ის შეცვლაც არ დაგვჭირდეს: `hwnd = 0` ნიშნავს „მთელ მონიტორს“, `width`/`height` მონიტორის ზომაა.
4. 50501-ზე ფრეიმები მიდის გეგმის მიხედვით (`send_frame`/`recv_frame` ჯერ შენი ჩეკლისტის ღია პუნქტია). `FrameHeader.codec` უკვე იღებს `"h264"`-საც.
5. B → `remote_mouse_move`, `mouse_button`, `mouse_wheel`, `key`. A `x_norm`/`y_norm`-ს ვირტუალური მონიტორის rect-ზე გადაითვლის და იყენებს `SendInput`-ს (`MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK`). ფოკუსის ხრიკები აღარ გვჭირდება, რადგან კლიკი ფანჯარას თავისით ააქტიურებს.

## ლუკამ უნდა გადაწყვიტოს

1. **დემოში თუ დემოს მერე?** CLAUDE.md-ში IddCx ახლა „არ შედის“ სიაშია.
2. **B-ს საკუთარი დესკტოპის KVM:** hotkey-ით (როგორც დემეტრეს ინსტრუქციაშია ნახსენები) თუ საერთოდ არა?
3. **Codec:** ჯერ JPEG და მერე H.264 (Media Foundation), თუ პირდაპირ H.264? H.264-ის შემთხვევაში სანდროს decoder იცვლება.
4. **`protocol.h`:** საკმარისია `stream_start.hwnd = 0` = „მთელი მონიტორი“ და კომენტარების შესწორება, თუ ცალკე ველები გინდა? ასევე: `remote_*`-ის `x_norm`/`y_norm` ითვლება სტრიმის (მონიტორის) მიმართ.

## სხვა, რაც თემოს სესიაში გაირკვა

- **VS 2026** (Community, ვერსია 18) შენს preset-ებთან მუშაობს: MSVC 19.51, CMake 4.3, Ninja 1.13, SDK 10.0.26100. build `/W4`-ზე უწარნინგოდ გადის, `queue_test` და `protocol_test` გადის. `setup_env.ps1` vswhere-ით ამ VS-ს იპოვის და Build Tools-ს აღარ დააყენებს.
- **`tools/*.exe` `host_a`-ს კოდს არ ლინკავს.** `capture_test.cpp` დროებით `#include "../host_a/window_picker.cpp"`-ს აკეთებს. თხოვნა: `host_a`-ს წყაროები (`main.cpp`-ის გარდა) static ბიბლიოთეკად, მაგალითად `kvm_host_lib`, რომელსაც `kvm_host` და tools ლინკავენ.
- **Windows PowerShell 5.1 piped stdin-ს თავში UTF-8 BOM-ს უმატებს** (გაზომილი: `EF BB BF 33 0D 0A`, cmd-ით მხოლოდ `33 0D 0A`). თუ რომელიმე exe stdin-იდან კითხულობს და ტესტში PowerShell-ით აწვდით, BOM წაშალეთ. `window_picker`-ში ეს უკვე გაკეთებულია.
- `window_picker` main-შია (`f5b4a58`). თემოს გადაწყვეტილებით review-ს გარეშე შევიდა, გადახედვა მაინც ღირს.

## ნუ

- ნუ შეცვლი თემოს ფაილებს: `host_a/window_picker.*`, `capture.*`, `jpeg_encoder.*`, `video_sender.*`, `tools/capture_test.cpp` და მომავალში `virtual_display.*`.
- ნუ დააყენებ დრაივერს ადმინით თავად. ბრძანება ლუკას მიეცი.
- თუ დრაივერი ხელს უშლის: სრულად იშლება Device Manager → Display adapters → Virtual Display Driver → Uninstall device-ით. შავი ეკრანის შემთხვევაში ეს Safe Mode-იდან გააკეთეთ. GPU დრაივერის დიდი განახლების წინ VDD წაშალეთ (დრაივერის ავტორების რჩევა).

## თემოს რა უნდა გაეგზავნოს

1. პასუხები 4 კითხვაზე.
2. დაყენდა თუ არა VDD A-ზე, მისი `\\.\DISPLAYn` და პოზიცია. ასევე A-ს ვიდეოკარტა (CLAUDE.md-ში ჯერ TODO-ა).
3. B-ს ეკრანის ზომა host-ის ლოგიდან (`hello`: `screen WxH`). ეს რეჟიმი `vdd_settings.xml`-ში უნდა იყოს.
4. პასუხი `kvm_host_lib`-ზე.
