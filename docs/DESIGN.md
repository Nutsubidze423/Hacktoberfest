DESIGN — Sarke (KVM + window streaming)

This document is the single source of truth for every pixel of the app. Follow it exactly. If something is not specified here, do not invent styling — ask.

Design direction (v2, 2026-10-18, Sandro: "matching design with sarke-nine.vercel.app"): the app looks like the Sarke website (D:\sarke-web, src/app/globals.css). Apple-like restraint: a neutral black / white / grey base, one blue for actions and links, solid rounded tiles, pill buttons, no borders or glows; Google Sans for text, Geist Mono for numbers. Dark and light themes, following Windows' app mode as the website follows the system. Colour lives only on status dots and the product's content.
(v1 was a hardware-instrument look: JetBrains Mono, uppercase labels, one orange accent, sharp corners. Surfaces not yet converted — the window picker (3) and the edge glow (5) — map their v1 tokens with the table in 2.2.)

0. Rules before anything else
Follow CONTRIBUTING.md: one task per branch, no direct commits to main. This design requires zero protocol changes.
First action in the first session that runs this prompt: save this document as docs/DESIGN.md (verbatim) in the same branch as your first design commit. Every later session reads docs/DESIGN.md, not this file.
No new vcpkg dependencies. Use Direct2D, DirectWrite, DWM, WIC from the Windows SDK. Link libraries with #pragma comment(lib, "d2d1.lib"), "dwrite.lib", "dwmapi.lib", "windowscodecs.lib" inside the .cpp that needs them, so CMakeLists (Luka's) does not need changes.
Hook callbacks (WH_MOUSE_LL, WH_KEYBOARD_LL) never draw, never create windows, never animate. They only push an event to a queue / PostMessage(WM_APP + n) to the UI thread.
All UI windows of a process live on one UI thread with one message loop. Network/capture threads communicate with it only via PostMessage or kvm::BlockingQueue / kvm::LatestSlot.
Never fake data. If a value is unknown (e.g. latency before clock offset exists), show — (U+2014). If reconnect logic does not exist, do not show "reconnecting".
Every value in this document lives in one tokens header (section 2). No magic numbers in drawing code.
1. Ownership of design work
Piece	Files (new unless noted)	Owner
Design tokens + drawing helpers	common/include/kvm/ui/tokens.h, common/include/kvm/ui/draw.h, common/src/ui_draw.cpp, assets/fonts/	Luka (owns common/)
DPI scaling used by UI	existing common/dpi.*	Demetre
Window picker overlay (host A)	host_a/window_picker.* (existing), host_a/picker_view.*	Temo
Viewer chrome, state screens, HUD (client B)	client_b/viewer_window.* (existing), client_b/viewer_chrome.*, client_b/viewer_hud.*	Sandro
Edge glow (both A and B)	common/include/kvm/ui/edge_glow.h, common/src/edge_glow.cpp	Demetre
Preview tools	tools/ui_preview_picker.cpp (Temo), tools/ui_preview_viewer.cpp (Sandro), tools/ui_preview_glow.cpp (Demetre)	per owner

The tokens and draw helpers live in client_b/ui_tokens.h and client_b/ui_draw.*; every surface uses them.

2. Design tokens (client_b/ui_tokens.h + client_b/ui_draw.*)
2.1 Units
Every size here is in dp (1 dp = 1/96 inch).
Convert with int px(float dp, UINT dpi) { return (int)lround(dp * dpi / 96.0); }.
Hairline = max(1, px(1, dpi)).
Processes are Per-Monitor-V2 DPI aware (see CONTRIBUTING.md). Create every Direct2D render target with dpiX = dpiY = 96 so 1 D2D unit = 1 physical pixel; scale manually with px(). Re-layout on WM_DPICHANGED and use the suggested rect from lParam.

2.2 Color — the website's palette (globals.css), two themes
Token	Dark	Light	Use (website variable)
bg	#050506	#FBFBFD	window background (--bg)
bg_alt	#0B0B0D	#F5F5F7	second background (--bg-alt)
tile	#1D1D1F	#F5F5F7	raised tiles: cards, panels, HUD, notification, status pill (--bg-raise)
fill	#2C2C2E	#E8E8ED	fills inside tiles: grey buttons, monitors, segmented track, keycaps (--bg-high)
fill_hi	#3A3A3C	#DCDCE1	hover inside tiles
seg_on	#545458	#FFFFFF	the selected segment of a segmented control
ink	#F5F5F7	#1D1D1F	primary text (--ink)
ink2	#A1A1A6	#424245	secondary text (--ink-2)
ink3	#86868B	#6E6E73	captions, labels, units (--ink-3)
accent	#0071E3	#0071E3	primary button fill (--accent); hover #0077ED
accent_ink	#2997FF	#0066CC	links, accent text, the crossing edge, selection outline (--accent-ink)
on_accent	#FFFFFF	#FFFFFF	text on accent
ok	#30D158	#248A3D	linked / healthy
warn	#FF9F0A	#B25000	degraded / connecting / unsaved
err	#FF453A	#D70015	lost / error
Lines are ink at 10 % (line) or 16 % (line_strong) alpha, never a solid grey. Halos (the website's body::before): two radial gradients, #6276A4 at 17 % from top-centre and #485474 at 11 % from bottom-right (light: #7896D2 16 %, #96A0C8 10 %), fading out at 70 % of the radius — on window backgrounds only.
Caption close button: Windows 11's own hover red #C42B1C with a white glyph.
Theme: Windows' app mode (HKCU\...\Themes\Personalize\AppsUseLightTheme); env SARKE_THEME=dark|light forces one. Every window repaints on WM_SETTINGCHANGE "ImmersiveColorSet".
v1 → v2 names (for surfaces 3 and 5): bg0→bg, bg1→tile, bg2→fill, line→ink 10 %, lineStrong→ink 16 %, textHi→ink, textMid→ink2, textLo→ink3, accent→accent_ink for lines/marks and accent for button fills, onAccent→on_accent.
Color is never the only signal: every status color is paired with a word.

2.3 Typography
Fonts (SIL OFL 1.1), bundled in assets/fonts/ with their licences, loaded privately with a DirectWrite font set (IDWriteFactory5::CreateFontSetBuilder + CreateFontCollectionFromFontSet), searched from the exe's folder upwards: GoogleSans-Regular/Medium/SemiBold/Bold.ttf (static instances of the website's Google Sans variable font, Latin + Latin Extended) and GeistMono-Regular/Medium/SemiBold.ttf. Fallbacks: Segoe UI Variable Text → Segoe UI; Cascadia Mono → Consolas (logged once).
Non-Latin text (Georgian, emoji in window titles) renders via DirectWrite system font fallback.
Text antialiasing: ClearType on opaque render targets; grayscale on layered/transparent surfaces.
Style	Size / line height	Weight	Font	Tracking
caption	12 / 16 dp	400	Google Sans	0
body	14 / 20 dp	400	Google Sans	-0.005 em
body_strong	14 / 20 dp	500	Google Sans	-0.005 em
title	17 / 22 dp	600	Google Sans	-0.01 em
headline	28 / 32 dp	700	Google Sans	-0.02 em
display	44 / 48 dp	700	Google Sans	-0.03 em
mono	12 / 16 dp	400	Geist Mono	0
mono_strong	13 / 18 dp	500	Geist Mono	0
Sentence case everywhere (no uppercase labels). Numbers, addresses, config values and keycaps use the mono styles.
Truncation: single line, ellipsis trimming. Never wrap titles. Never clip glyphs vertically.

2.4 Spacing, shape, motion
Spacing scale: 4, 8, 12, 16, 24, 32, 48 dp.
Radii (the website's --r-*): xs 6 (monitors, keycaps, small chips), sm 12 (HUD), md 18 (cards, notification, stalled card), lg 28 (big tiles: map, panel, Identify). Buttons, toggles, status pills and segmented controls are pills (radius = height / 2).
Windows: Windows 11 rounded corners (DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUND), no 1 px system border (DWMWA_BORDER_COLOR = DWMWA_COLOR_NONE), DWMWA_USE_IMMERSIVE_DARK_MODE per theme. Popups with their own shape (notification, Identify) are WS_EX_LAYERED with per-pixel alpha (UpdateLayeredWindow) so their rounded corners are really transparent.
No borders around tiles; tiles are separated by fill only (the notification card alone has a 10 % ink hairline and a soft shadow because it floats over any app).
Buttons: primary = accent pill, on_accent text (body_strong); secondary = fill pill, ink text; link = accent_ink text with "›"; disabled = 38 % alpha. Segmented control = fill pill track (2 dp inset) with pill segments; selected segment = seg_on, ink text; others ink2, hover fill_hi.
Motion: the website's easing (0.25, 0.1, 0.25, 1) at 180 ms where something moves. Allowed: connecting-dot blink, the notification card's slide + fade (9.3), the edge glow (5). Hover and selection are instant. Reduced motion (SPI_GETCLIENTAREAANIMATION FALSE): no blinking; the card only fades.

2.5 Copy rules
Sentence case; short, plain words, like the website ("Where is the other PC?", "Connection lost", "Reconnecting by itself.").
Separator: · (space, U+00B7, space). Dimensions: 1920×1080 (U+00D7). Units after the value with one space: 14 ms, 12.4 Mbps, 1080 px.
Links end with › (U+203A). Hotkeys are drawn as keycaps (mono on fill, radius xs): Ctrl Alt Shift W.
Unknown values are — (U+2014). Never fake data.

3. Surface 1 — Window picker overlay (host A) — Temo

Trigger: global hotkey Ctrl+Alt+Shift+S via RegisterHotKey(MOD_CONTROL|MOD_ALT|MOD_SHIFT|MOD_NOREPEAT, 'S'). The keyboard hook must pass this combo through when control is on A. Ctrl+Alt+Shift+Q (emergency) is never touched.

Window: WS_POPUP, WS_EX_TOPMOST | WS_EX_TOOLWINDOW, class style CS_DROPSHADOW. Opaque D2D ID2D1HwndRenderTarget. Appears instantly (no fade). Closes on ESC, on selection, and on WM_ACTIVATE → WA_INACTIVE.

Placement: monitor containing the cursor; horizontally centered in its work area; top edge at 22% of work-area height.

Size: width 640 dp. Height = 56 (search) + 1 (divider) + list + 1 (divider) + 32 (footer). List = min(rows, 8) × 56 dp; empty state list height = 112 dp.

Panel: fill bg1; outer border hairline lineStrong (also set DWMWA_BORDER_COLOR to lineStrong on Windows 11).

3.1 Search row (height 56)
> at x=16, title style, accent, vertically centered.
Filter text at x=36, title style, textHi. Placeholder when empty: filter windows in textLo.
Caret: filled rect 2×18 dp, accent, right after the last character (+2 dp), vertically centered, blinking.
Text input is self-drawn (no EDIT control): WM_CHAR appends printable chars, Backspace deletes last char, Ctrl+Backspace clears. No cursor movement.
Right side, right edge at width−16: {N} WINDOWS in label, textLo.
Filter: case-insensitive substring match on title OR process name. Selection resets to first match on every change.
3.2 List rows (height 56)
Thumbnail cell 64×40 dp at x=16, y=8. Cell fill bg0, hairline line border drawn outside the cell. Live preview via DwmRegisterThumbnail + DWM_TNP_SOURCECLIENTAREAONLY, scaled to fit the cell preserving aspect, centered. Register only for visible rows; unregister on scroll/close.
Text column starts at x=92.
Line 1, top at y=10: window title, body, textHi, ellipsis.
Line 2, top at y=30: {PROCESS.EXE} · {W}×{H}, label, textLo. Append  · MINIMIZED if minimized.
Text column right limit: width − 16 − 84 (space for the hint is reserved on every row, so text never reflows on selection).
Selected row: fill bg2 full row width; 2×56 dp accent bar at x=0. Right hint ENTER STREAM, label, accent, right edge at width−16, vertically centered.
Currently streamed window: line 2 ends with  · LIVE where LIVE is drawn in accent.
Rows separated by hairline line from x=16 to width−16 (not under the last row).
Scroll indicator: 2 dp wide lineStrong bar on the right edge, only when rows > 8, height proportional.
3.3 Which windows to list

Visible, top-level, non-empty title, not WS_EX_TOOLWINDOW, not cloaked (DWMWA_CLOAKED = 0), not owned by this process. Include minimized windows (marked). Order = EnumWindows Z-order. On selecting a minimized window: ShowWindow(SW_RESTORE) before starting capture (WGC cannot capture minimized windows).

3.4 Empty state

NO MATCH in label, textLo, centered in the 112 dp list area.

3.5 Footer (height 32)
Left at x=16: UP/DOWN NAVIGATE   ENTER STREAM   ESC CLOSE (three spaces between groups), label, textLo; the key words (UP/DOWN, ENTER, ESC) in textMid.
Right edge at width−16: KVM HOST, label, textLo.
3.6 Input

Up/Down move selection (clamped, no wrap), PageUp/PageDown ±8, Home/End, Enter = stream, Esc = close. Mouse hover = selection, click = stream, wheel = 1 row per notch.

4. Surface 2 — Viewer window (both PCs) — Sandro
4.1 Renderer
The whole client area is drawn by one Direct2D device context (dpi 96) on a D3D11 flip-model swapchain (DXGI_SWAP_EFFECT_FLIP_DISCARD, a frame-latency waitable object, latency 1), on the GPU or on WARP without one (client_b/swapchain_target.h). A decoded JPEG frame is uploaded with ID2D1Bitmap::CopyFromMemory into a persistent bitmap (recreated only on size change); a decoded H.264 picture is already a BGRA texture on the same device and is drawn as is. No GDI in the window.
Interpolation: if the drawn image size equals the frame size → NEAREST_NEIGHBOR (pixel-exact). Otherwise → LINEAR.

4.2 Frame chrome (custom title strip)
Window style WS_OVERLAPPEDWINDOW; the standard frame is removed in WM_NCCALCSIZE (return 0 when wParam == TRUE) so DWM shadow, Snap and resize keep working; when maximized the client rect is inset by SM_CXFRAME + SM_CXPADDEDBORDER. style_window(): rounded corners, no system border, theme-matched non-client parts.
WM_NCHITTEST: 6 dp resize borders → HTLEFT/HTTOPRIGHT/etc.; title strip (except buttons) → HTCAPTION.
Title strip: height 44 dp, fill bg, no line under it.
Left: the Sarke mark 20 dp at x = 14, centred; 8 dp; "Sarke" (body_strong, ink); 12 dp; status pill (26 dp, tile): 7 dp dot in the state color + state word (caption, ink2) with 10 dp padding; 12 dp; the stream's window title (body, ink2, ellipsis) when streaming.
Right, right to left: close, maximize/restore, minimize — 46×44 dp each, Windows 11 glyphs (10 dp, hairline; restore and maximize squares softly rounded 2 dp). Hover fill tile, glyph ink; close hover #C42B1C with a white glyph. Then 8 dp, then the toggles Fit and HUD: pills 26 dp high, as wide as the widest label ("HUD", "Fit", "1:1") + 2 × 10 dp, 4 dp apart; on = fill + ink, off = ink2 text only, hover = tile.
HUD toggles the HUD. Fit toggles fit ↔ 1:1 (label "1:1" while in 1:1). In 1:1 the frame is centred and cropped, no scrolling.

4.3 Content area
Fill bg. Fit mode: frame scaled to fit preserving aspect, centred; letterbox bars are bg.
Input mapping: normalized coordinates are computed against the drawn image rect. Mouse events over the letterbox are ignored and not sent.
Initial size before any stream: client 960×600 dp. On stream_start: frame size scaled to fit 80 % of the work area, + 44 dp strip, centred. Minimum client size 480×320 dp.

4.4 Connection states (no live frames) — centred like the website's hero
State	Dot	Pill word	Headline	Detail (body, ink2)
Waiting	ink3	Waiting	Waiting for the other PC	Looking for {ip}:{port} (or "It can start any time.")
Connecting	warn, blinking	Connecting	Connecting	Connecting to {ip}:{port}
Linked, no stream	ok	Linked	Linked	Round trip {n} ms + the hint row below
Streaming	accent_ink	Streaming	— (frames shown)	—
Stalled (no frame > 1000 ms)	warn	Stalled	card "Stalled" (headline, warn)	{t} s without frames (1 decimal)
Lost	err	Connection lost	Connection lost	Reconnecting by itself. (only if reconnect exists; else "Restart Sarke on this PC.")
Layout, centred horizontally and vertically over the halo background: "Sarke" (title, ink2); 8 dp; headline in display (or headline style if display doesn't fit the width), ink; 16 dp; detail line(s).
Linked hint, 24 dp below: "On the other PC, press" [Ctrl] [Alt] [Shift] [W] "to send a window" — keycaps 26 dp high, mono on tile, radius xs, 8 dp padding, 4 dp apart. If it doesn't fit, one plain line instead.
Footer, centred, 24 dp above the bottom: {hostname} · {W}×{H} · {scale}% (caption, ink3).
Stalled: keep the last frame, bg at 62 % over it, then a centred tile (radius md, 24 dp padding) with "Stalled" (headline, warn) and the detail line.

4.5 HUD (overlay on content, top-right)
Visible by default. top = strip bottom + 12 dp, right = 12 dp. Fixed width 188 dp; 10 dp vertical / 14 dp horizontal padding; rows 22 dp. Tile at 90 % alpha, radius sm, no border.
Rows: label left (caption, ink3); value right (mono_strong, ink), unit after it (caption, ink3).
Label	Value	Warn	Err
RTT	{n} ms from ping/pong	> 20	> 50
FPS	displayed frames/s, 1 s window	< 24	< 12
Latency	capture → display in ms using the clock offset; — until known	> 80	> 150
Bitrate	video payload, 1 decimal, Mbps	—	—
Resolution	{W}×{H}	—	—
Codec	of the frames shown: "H.264", or "JPEG 75" with the quality from config	—	—
Dropped	frames skipped since stream start	—	—
Value color becomes warn/err per thresholds; otherwise ink. Updated at 4 Hz.
5. Surface 3 — Edge glow (A and B) — Demetre

Shown on the destination machine when control crosses: on B when focus_enter arrives, on A when control returns.

Window: WS_POPUP, WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, shown with SW_SHOWNOACTIVATE. Never takes focus, never receives clicks.
Render: ID2D1DCRenderTarget bound to a 32-bpp premultiplied DIB section → UpdateLayeredWindow(ULW_ALPHA).
Geometry: on the entry edge of the monitor. Segment length 240 dp centered on the entry point (y_norm × monitor height), clamped inside the monitor. Solid accent bar 3 dp thick on the edge pixel column, plus a 24 dp linear gradient inward from accent at 35% alpha to 0%.
Timing: fade in 80 ms (ease-out), hold 320 ms, fade out 420 ms (ease-in). Reduced motion: static 600 ms.
A new crossing during the animation restarts it at the new position.

P2 — edge resistance (only after M2): switching requires 24 dp of accumulated push past the edge within 400 ms; while pushing, glow alpha = push / 24. Config key edge_resistance_dp (0 = off, default 0). 16 dp dead zone at the top and bottom of the edge.

6. Priority (do not let design endanger the demo)
P0 (with or right after M1): tokens + draw helpers; viewer renderer, title strip, letterbox-correct input mapping, state screens; HUD.
P1 (after M1 works): picker overlay with live thumbnails; edge glow.
P2 (after M2 only): edge resistance, 1:1 mode, tray icon, app icon.

If a P0 functional task of your track is not done, do it first. Design never blocks function.

7. Verification (required before marking any design task [x])
Each owner builds a preview tool (tools/ui_preview_*.cpp) that renders their surface in every state with fake data, no network needed. The viewer preview must cycle Waiting → Connecting → Linked → Streaming (with a test image) → Stalled → Lost.
Check at Windows display scale 100%, 125%, 150%, 200% (B's laptop is likely 150%):
every hairline is exactly max(1, px(1)) physical pixels and not blurred (zoom a screenshot to 800%);
no text clipped or overlapping; ellipsis appears on long titles;
all positions match this spec within 0 px after px() rounding;
HUD width does not change when values change.
Test a window titled in Georgian and one with an emoji in the picker.
Test reduced motion (Settings → Accessibility → Visual effects → Animation effects off).
Test WM_DPICHANGED: drag the viewer between monitors with different scale.
After the change: explain what was done and how to run the preview tool.
8. Do not
Do not add borders around tiles, glows (except the edge glow), glass/acrylic, gradients other than the halos, or a second accent color.
Do not use uppercase labels or the v1 orange.
Do not use GDI for text or shapes anywhere UI is drawn.
Do not use system EDIT/BUTTON controls in styled surfaces.
Do not animate anything not listed in 2.4.
Do not hardcode colors/sizes outside tokens.h.
Do not touch another developer's files; if your surface needs something from theirs, tell the user who to ask.

Future design changes will be made by editing docs/DESIGN.md and tokens.h only — and the website (D:\sarke-web) stays the reference: when its globals.css changes, change tokens.h to match.

9. App logo, tray icon and notifications
App name: Sarke — Georgian სარკე, "mirror". Written "Sarke" everywhere (window title, tray, notifications, strip). One place in code: tokens (kAppName).
Mark (v2, 2026-10-18 — the website's src/app/icon.tsx; replaces the v1 "ს caret"): one desktop mirrored into the next. A rounded tile (#050506, radius 22 % of its size); a solid #F5F5F7 rounded square top-left (44 % of the tile, inset 17 %, radius 10 %); an outlined square of the same size bottom-right (inset 17 %, stroke 7 %, filled #050506) overlapping it. The same colours in both themes. Fractions live in tokens.h (logo::).
Rendering: drawn with Direct2D (antialiased) at the exact pixel size Windows asks for (SM_CXSMICON / SM_CXICON for the current DPI) into a premultiplied canvas, transparent outside the rounded tile, converted to a 32-bpp icon. In windows it is Painter::logo(). Check with tools/ui_preview_logo.exe; docs/brand/sarke.ico (16–256 px) is generated from it (ui_preview_logo --ico).
Uses: window icons, tray icon, title strips, notification card, Identify.
Tray icon: in the notification area ("show hidden icons"). Tooltip: Sarke · {state}. Left click or double click opens the viewer; right click: Show viewer, Display arrangement... (when the app provides it), Quit.
Notifications: Sarke's own card (9.3), not the Windows toast; the Windows one is only the fallback if the card can't be drawn. Connected: title "Connected to host", text "{ip}:{port}". Lost: title "Connection lost", text "reconnecting" (only if reconnect exists, otherwise "restart the client"). No notification for Waiting/Connecting.

9.3 Notification card (2026-10-18, Sandro: "like NVIDIA's, top-right, our own design"; v2 = the website's tiles). Code: client_b/toast.* (kvm::Toast), preview: tools/ui_preview_toast.exe.
Where: top-right of the main screen's work area, 16 dp from the top and right. One card at a time: a new one replaces the text of the one showing and restarts its time. Topmost, never takes focus, not in the taskbar or Alt+Tab. WS_EX_LAYERED with per-pixel alpha: the rounded corners and the shadow are transparent.
Card: 380 dp wide, height = 16 + max(40, caption + 2 + title + body) + 8 + 16 dp; tile fill, radius md, a 10 % ink hairline, soft shadow (14 dp of faint black rings, offset down).
Logo: the mark at 40 dp, 16 dp from the left, centred.
Text, 14 dp after the logo: row 1 = 7 dp dot (ok = connected, err = lost, accent_ink = anything else) + "Sarke" (caption, ink) + " · {tag}" (caption, ink3); 2 dp; title (title, ink); text (body, ink2). Single-line with ellipsis.
Close: × 8 dp glyph in a 24 dp circle, 10 dp from the top-right corner; ink3, hover fill_hi circle + ink.
Time line: a 2 dp pill 8 dp above the bottom edge, 16 dp in from both sides: track ink 10 %, run accent_ink, shrinking over the hold time (5 s); while hovered it stops and dims to 40 %.
Motion: in = slide 32 dp from the right + fade, 220 ms ease-out; out = the reverse, 180 ms ease-in; short so it never shows on a monitor to the right. With Windows' animation effects off: fade only.
Behaviour: hover holds it; click anywhere but × = open (e.g. show the viewer) and close; × = close.

10. Display arrangement window (2026-10-18, Sandro). Code: client_b/display_layout.* (logic, tested by tools/display_layout_test.exe) + client_b/display_settings.* (window); standalone: tools/sarke_displays.exe. Browser preview: docs/preview/sarke-app.html, Displays.
What it sets: config.json client_side, the only arrangement the apps use. The KVM maps this PC's whole outer edge on that side onto the other PC's edge (kvm::display::OuterEdge), so there is no offset or alignment to choose. On the host client_side = the side the other PC is on; on the client it is the opposite. Each PC has its own config.json, so the status line says what the other PC must say.
Window: frameless, rounded, 980×700 dp (min 820×640), bg with the halos. Title strip 44 dp: the mark 20 dp, "Sarke" (body_strong, ink), "›  Displays" (body, ink2); minimize and close (as 4.2).
Header (40 dp side padding): caption "Settings · Displays" (ink3); headline "Where is the other PC?" (ink); two body lines (ink2).
Map (tile, radius lg, 20 dp padding): top-left a green 7 dp dot + "Live · n monitors · W×H" (caption, ink3). This PC's real monitors (kvm::display::monitors() minus the Virtual Display Driver), fill with radius ≤ 9 dp, number (title), resolution (caption, ink2), scale (+ " · Main", caption, ink3). The other PC at its hello size (or the last one seen, %LOCALAPPDATA%\Sarke\peer.json) right outside the desktop, centred on the crossing edge: fill (fill_hi on hover/drag) with a 2 dp accent_ink outline, "Other PC" (caption, accent_ink), hostname (body_strong), size · scale (caption, ink2), Connected (ok) / Last seen / Not connected yet (caption, ink3). The crossing edge = the same pieces the KVM hook uses: a 3 dp accent_ink pill on this PC's side of each piece, no glow. One scale for this PC plus the other PC on every allowed side, so nothing jumps while dragging.
Drag the other PC: it follows the pointer (centre kept on the map), the allowed drop zones show as 1.5 dp ink 16 % outlines (radius xs) and the nearest one lights up (accent 16 % fill + accent_ink outline); the edge and the panel preview the drop. Arrow keys pick a side. Enter = Apply, Esc = cancel drag / revert / close.
Panel (300 dp tile, radius lg, 20 dp padding): caption "Other PC is" + segmented [Left][Right][Top][Bottom] (Top / Bottom disabled with "Top and bottom: next host update." until host_a/main.cpp and client_b/main.cpp accept them; then flip kVerticalSidesAccepted); caption "This PC is the" (+ " (guessed)") + segmented [Host][Client] (guessed from the running app, then host_ip; locked when the app passes its role); readout rows (30 dp, ink 10 % separators): Mouse crosses ("Right edge", body_strong), Crossing edge ("1080 px", mono_strong), client_side (exactly what Apply writes, mono_strong), Status (Saved / Not saved in warn / Saved in ok just after Apply / Default / Error in err); Apply (primary pill) and Revert (secondary pill), 36 dp, disabled while nothing changed.
Footer: the status line (body; warn / ok / err / ink2), then the config path (mono, ink3), Identify (secondary pill) and "Windows display settings ›" (link, opens ms-settings:display).
Identify: on each real monitor, 48 dp from its bottom-left corner, a 200 dp rounded tile (radius lg, layered) with the number (96 dp, bold), the mark + "Sarke" and "W×H · scale"; 2.5 s, click to hide.
Live: re-reads the monitors on WM_DISPLAYCHANGE, DPI and setting changes and every 2 s; re-reads config.json and peer.json when the window is activated (unless there are unsaved changes); repaints on a theme change. Apply keeps every other key of config.json and its order and never overwrites a file it can't parse.

11. Launcher: sarke.exe (2026-10-18, Sandro). Code: client_b/launcher.*, tools/sarke.cpp. Behaviour: docs/archive/hackathon/BLUEPRINT.md 2.0 and 3.3.
No window of its own: the tray icon (the mark), its menu and the notification cards (9.3) are its whole face; Display arrangement (10) opens from it.
Tooltip: "Sarke · {Host|Client} · {state}" — states: Starting, Waiting for the other PC (host) / Looking for the host (client), Linked to {PC} · {rtt} ms, Connection lost, Stopped.
Menu (Windows' own menu, sentence case, "…" on items that open a window): Sarke · This PC is the {role} (disabled) — Display arrangement… (default, bold) — Window quality ▸ — Frame rate ▸ — Desk mode (the other PC as a monitor) — Notifications ▸ — Open received files — Open config.json — Show log — This PC is the ▸ — Restart Sarke — Quit Sarke. Radio dots for one-of choices, check marks for on/off.
Cards (title / text, dot colour): Sarke is running / "{Host|Client} · settings are in the tray" (accent_ink) · Connected to {PC} / "{ip:port} · {W}×{H} · {scale}%" (ok) · Connection lost / "Waiting for {PC} to come back." or "Reconnecting to {PC}…" (err) · Sarke has a problem / the app's error line (err, at most every 10 s) · Sarke stopped / why (err) · Restarting Sarke / "{what changed} — back in a moment." (accent_ink) · Sarke is already running / "It lives in the tray (^ on the taskbar)." (accent_ink).
