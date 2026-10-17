# CLAUDE.md — KVM + Window Streaming დემო

## კომუნიკაცია
- მომხმარებელთან **ქართულად** ისაუბრე. კოდი, კომენტარები, ცვლადების სახელები და commit-ები: **ინგლისურად**.
- მომხმარებელი AI-ს დახმარებით სწავლობს და აწყობს პროექტს. ცვლილების შემდეგ მოკლედ ახსენი, რა გააკეთე და როგორ გაუშვას/შეამოწმოს.
- ერთდროულად ერთ ეტაპზე იმუშავე. ეტაპის დასრულების შემდეგ განაახლე ჩეკლისტი ამ ფაილში.

## პროექტის მიზანი
ორი Windows კომპიუტერი ერთ ლოკალურ ქსელში:
- **A** — სახლის PC, 2 მონიტორი. ფლობს ფიზიკურ მაუსს და კლავიატურას. ეშვება **host** როლში.
- **B** — ლეპტოპი, დგას A-ს გვერდით. ეშვება **client** როლში.

საბოლოო მიზანი: A-ს მაუსი/კლავიატურა გადადის B-ს ეკრანზე (როგორც Deskflow / Mouse Without Borders), და A-ს ცალკეული ფანჯარა (მაგ. Photoshop) ტრანსლირდება B-ზე ისე, თითქოს B იყოს A-ს მესამე მონიტორი. ფანჯრის პროცესი ყოველთვის A-ზე მუშაობს; B მხოლოდ აჩვენებს სურათს და აბრუნებს ინფუთს.

## დემოს ფარგლები (10 საათი)
**შედის:**
1. მაუსი/კლავიატურა A → B ეკრანის კიდიდან გადასვლით (B-ს კონტროლი).
2. A-ზე არჩეული ერთი ფანჯრის სტრიმი B-ზე ცალკე ფანჯარაში.
3. სტრიმულ ფანჯარაზე კლიკები/სქროლი/კლავიატურა → A-ს ორიგინალ ფანჯარას.

**არ შედის (მერე):** ვირტუალური მონიტორის დრაივერი (IddCx), ფანჯრის drag-ით გადატანა, დაშიფვრა/დაწყვილება, clipboard, ფაილები, ინსტალერი, პენის წნევა.

## ტექნოლოგიები
- Python 3.11+, მხოლოდ Windows 10/11.
- `pynput` — global mouse/keyboard hooks (A).
- `ctypes` + Win32 `SendInput`, `SetCursorPos`, `SetForegroundWindow` — ინფუთის ემულაცია (A და B).
- `windows-capture` (Windows.Graphics.Capture) — ერთი ფანჯრის გადაღება; fallback: `dxcam` + ფანჯრის rect crop.
- კომპრესია: დასაწყისში **JPEG** (`turbojpeg` ან `opencv-python`). შემდეგ, თუ დრო დარჩა: `PyAV` + `h264_nvenc` / `h264_qsv` / `h264_amf`.
- ქსელი: `asyncio` TCP.
- B-ზე ჩვენება: `pygame` (ან `tkinter` + PIL).
- `pywin32` — ფანჯრების სია, HWND, rect, DPI.

A-ს ვიდეოკარტა: _TODO — შეავსე (NVIDIA / AMD / Intel)_

## სტრუქტურა
```
kvm-demo/
  CLAUDE.md
  requirements.txt
  config.json              # IP-ები, პორტები, B-ს მდებარეობა (left/right), ხარისხი
  common/
    protocol.py            # შეტყობინებების ტიპები, encode/decode
    net.py                 # length-prefixed TCP framing
    win32_input.py         # SendInput wrappers
    dpi.py                 # SetProcessDpiAwareness, მონიტორების გეომეტრია
  host_a/
    main.py                # გაშვება: python -m host_a.main
    input_capture.py       # hooks + edge detection + ინფუთის გადამისამართება
    window_picker.py       # ხილული ფანჯრების სია, არჩევა
    window_stream.py       # capture → encode → send
    remote_input.py        # B-დან მოსული ინფუთის ჩასმა A-ს ფანჯარაში
  client_b/
    main.py                # გაშვება: python -m client_b.main
    input_inject.py        # A-დან მოსული ინფუთის ემულაცია B-ზე
    viewer.py              # სტრიმის ჩვენება + ინფუთის უკან გაგზავნა
  tools/
    ping_test.py           # კავშირისა და latency-ს ტესტი
```

## ქსელური პროტოკოლი
- A არის სერვერი, B უკავშირდება.
- პორტი **50500** — control + input (მცირე JSON შეტყობინებები, 4-ბაიტიანი length prefix).
- პორტი **50501** — ვიდეო ფრეიმები (header JSON + binary payload, length-prefixed).
- `Nagle` გამორთე (`TCP_NODELAY`) ორივე სოკეტზე.

შეტყობინებები (`type` ველით):
| type | მიმართულება | ველები |
|---|---|---|
| `hello` | B→A | `screen_w`, `screen_h`, `dpi_scale`, `hostname` |
| `focus_enter` / `focus_leave` | A→B | `y_norm` (კურსორის შესვლის წერტილი) |
| `mouse_move` | A→B | `dx`, `dy` (relative) |
| `mouse_button` | ორივე | `button`, `down` (+ B→A-ზე `x_norm`, `y_norm`) |
| `mouse_wheel` | ორივე | `delta`, `horizontal` |
| `key` | ორივე | `vk`, `scan`, `down`, `extended` |
| `stream_start` / `stream_stop` | A→B | `hwnd`, `title`, `width`, `height` |
| `remote_mouse_move` | B→A | `x_norm`, `y_norm` (სტრიმულ ფანჯარაში, 0..1) |
| `ping` / `pong` | ორივე | `t` |

## მნიშვნელოვანი წესები
- **DPI:** ორივე პროცესი დაიწყოს `SetProcessDpiAwareness(2)`-ით (per-monitor). ყოველთვის ფიზიკურ პიქსელებში იმუშავე.
- **კოორდინატები სტრიმულ ფანჯარაზე:** B აგზავნის ნორმალიზებულ (0..1) კოორდინატებს. A გადაითვლის ფანჯრის **client area**-ს მიმდინარე rect-ით (ფანჯარა შეიძლება გადაადგილდეს).
- **Hook-ები:** როცა კონტროლი B-ზეა, A-ს ლოკალური ინფუთი უნდა დაიბლოკოს (suppress), ხოლო კურსორი A-ზე ეკრანის ცენტრში ჩაიკეტოს და relative მოძრაობა გაიგზავნოს.
- **გადაუდებელი გამოსვლა:** `Ctrl+Alt+Shift+Q` ყოველთვის აბრუნებს კონტროლს A-ზე და აჩერებს hook-ებს. ეს არასოდეს წაშალო.
- Hook callback-ებში მძიმე სამუშაო არ გააკეთო — მხოლოდ queue-ში ჩადება.
- ვიდეო: თუ ქსელი ვერ ასწრებს, ძველი ფრეიმები გადააგდე (ყოველთვის ბოლო ფრეიმი), queue არ დააგროვო.
- ელევირებული (Admin) ფანჯრებში ინფუთი ჩვეულებრივი პროცესიდან არ გადის — თუ საჭიროა, host ადმინით გაუშვი.

## გაშვება
```
pip install -r requirements.txt
# A-ზე:
python -m host_a.main
# B-ზე:
python -m client_b.main --host <A-ს IP>
```
Windows Firewall-ში დაუშვი Python პორტებზე 50500–50501 (Private network).

## ჩეკლისტი
- [ ] 0. გარემო: Python, venv, requirements, firewall, `tools/ping_test.py` მუშაობს ორივე მხარეს
- [ ] 1. `common/net.py` + `protocol.py` — კავშირი, hello, ping/pong, ხელახალი დაკავშირება
- [ ] 2. A: hooks + edge detection; B: მაუსის მოძრაობა და კლიკები
- [ ] 3. კლავიატურა + სქროლი + გადაუდებელი ღილაკი + A-ზე დაბრუნება
- [ ] 4. A: ფანჯრების სია და არჩევა; ერთი ფანჯრის capture (ფაილში შენახვით ტესტი)
- [ ] 5. JPEG სტრიმი → B viewer (FPS და latency ეკრანზე)
- [ ] 6. B viewer → A: მაუსი/კლიკები/სქროლი სტრიმულ ფანჯარაზე
- [ ] 7. B viewer → A: კლავიატურა, ფოკუსი
- [ ] 8. ტესტი Photoshop-ით; latency/ხარისხის მორგება (სურვილისამებრ NVENC)

## შემდეგი ეტაპები (დემოს მერე)
IddCx ვირტუალური მონიტორი A-ზე B-ს ზომით → ფანჯრის drag-ით გადატანა → B-ზე ფანჯრების overlay იმავე პოზიციაზე → TLS + PIN დაწყვილება → clipboard → C++/Rust-ზე გადატანა წარმადობისთვის.
