DESIGN PROMPT — KVM + Window Streaming (paste into Claude Code)

You are implementing the visual design of this project. This document is the single source of truth for every pixel. Follow it exactly. If something is not specified here, do not invent styling — ask.

Design direction: hardware-instrument aesthetic (Teenage Engineering: monospace, uppercase labels, one loud orange accent, sharp corners, readouts) + command-palette interaction (Raycast: hotkey overlay, type-to-filter, keyboard-first).

0. Rules before anything else
Obey CLAUDE.md: file ownership, branch naming, one task at a time, no direct commits to main. Never edit another developer's files. Never edit common/include/kvm/protocol.h unless you are in Luka's session. This design requires zero protocol changes.
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

If tokens/draw helpers do not exist yet when you start: create a temporary copy inside your own files (e.g. client_b/ui_tokens_tmp.h), tell the user it must be replaced once Luka's version lands, and continue.

2. Design tokens (tokens.h)
2.1 Units
Every size here is in dp (1 dp = 1/96 inch).
Convert with int px(float dp, UINT dpi) { return (int)lround(dp * dpi / 96.0); }.
Hairline = max(1, px(1, dpi)).
Processes are Per-Monitor-V2 DPI aware (per CLAUDE.md). Create every Direct2D render target with dpiX = dpiY = 96 so 1 D2D unit = 1 physical pixel; scale manually with px(). Re-layout on WM_DPICHANGED and use the suggested rect from lParam.
2.2 Color (sRGB hex, alpha 0–1 where noted)
Token	Value	Use
bg0	
#0B0B0C	window base, letterbox, HUD base
bg1	
#141416	panels (picker)
bg2	
#1C1C1F	selected/hover row, button hover
line	
#2A2A2E	1px dividers, borders
lineStrong	
#3A3A40	panel outer border, scroll indicator
textHi	
#EDEDED	primary text, values
textMid	
#B4B4BA	secondary text, icon glyphs
textLo	
#8A8A92	labels, hints, units
accent	
#FF5A1F	the one accent: live state, selection bar, caret, glow
onAccent	
#0B0B0C	text on accent fill
ok	
#3DDC84	linked / healthy
warn	
#FFC21A	degraded / connecting
err	
#FF4D4D	lost / error, close-button hover

Contrast (checked, WCAG): textLo on bg0 5.7:1, on bg2 4.9:1. accent on bg0 6.3:1. onAccent on accent 6.3:1. Never put textLo on any color lighter than bg2. Color is never the only signal: every status color is paired with a text word.

2.3 Typography
Font: JetBrains Mono (SIL OFL 1.1), weights Regular (400) and Bold (700). Ship JetBrainsMono-Regular.ttf, JetBrainsMono-Bold.ttf and OFL.txt in assets/fonts/.
Load privately with DirectWrite custom font set (IDWriteFactory5::CreateInMemoryFontFileLoader or CreateFontSetBuilder + CreateFontCollectionFromFontSet). Embedding as an RC resource keeps "exe + config.json" deployment; if that needs a CMake change, ship the fonts next to the exe and ask Luka. Fallback if loading fails: Consolas, and log it once.
Non-Latin text (Georgian, Cyrillic, emoji in window titles) must render via DirectWrite system font fallback. Test with a window whose title is Georgian.
Text antialiasing: ClearType on opaque render targets; grayscale on any layered/transparent surface.
Style	Size / line height	Weight	Case	Letter spacing
label	11 / 14 dp	Regular	UPPERCASE	+0.06 em (IDWriteTextLayout1::SetCharacterSpacing)
body	13 / 18 dp	Regular	as written	0
title	15 / 20 dp	Regular	as written	0
readout	13 / 18 dp	Bold	—	0
display	28 / 32 dp	Bold	UPPERCASE	+0.02 em

Truncation: single line, ellipsis trimming (CreateEllipsisTrimmingSign). Never wrap titles. Never clip glyphs vertically.

2.4 Spacing, shape, motion
Spacing scale: 4, 8, 12, 16, 24, 32, 48 dp. Nothing else.
Corners: 0 everywhere. Set DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_DONOTROUND on every window (Windows 11; harmless no-op on Windows 10).
Borders: drawn as filled hairline rectangles at integer pixel coordinates. Never use DrawRectangle/DrawLine for axis-aligned lines (they blur on half pixels).
Motion is minimal and crisp. Allowed animations only: caret blink, connecting-status blink, edge glow. Everything else (hover, selection, open/close) is instant.
Blink period = GetCaretBlinkTime() (if it returns INFINITE, do not blink).
Reduced motion: if SystemParametersInfo(SPI_GETCLIENTAREAANIMATION) returns FALSE → no blinking, edge glow shows static for 600 ms without fades.
Easing for glow fades: ease-out cubic 1 - (1-t)^3 on fade-in, ease-in cubic t^3 on fade-out.
2.5 Copy rules
Labels UPPERCASE. Detail lines lowercase sentence style, no trailing period.
Separator: · (space, U+00B7, space). Dimensions: 1920×1080 (U+00D7, no spaces).
Numbers integers unless stated. Unit after value, separated by one space, unit drawn in textLo + label style.
No arrow glyphs in UI copy; use words (UP/DOWN, ENTER, ESC).
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

4. Surface 2 — Viewer window (client B) — Sandro
4.1 Renderer

The whole client area is rendered by one ID2D1HwndRenderTarget (dpi 96). Each decoded BGRA frame is uploaded with ID2D1Bitmap::CopyFromMemory into a persistent bitmap (recreate only on size change). This replaces StretchDIBits from the v1 plan — confirm with Sandro before switching; mixing GDI and D2D in one window is not allowed (flicker).

Interpolation: if the drawn image size equals the frame size → NEAREST_NEIGHBOR (pixel-exact). Otherwise → LINEAR.

4.2 Frame chrome (custom title strip)
Window style WS_OVERLAPPEDWINDOW; remove the standard frame by handling WM_NCCALCSIZE (return 0 when wParam == TRUE) so DWM shadow, Snap and resize keep working. When maximized, inset the client rect by GetSystemMetricsForDpi(SM_CXFRAME) + SM_CXPADDEDBORDER (and Y equivalents) so content is not cut off.
WM_NCHITTEST: 6 dp resize borders on all sides → HTLEFT/HTTOPRIGHT/etc.; title strip (except buttons) → HTCAPTION.
Title strip: height 32, fill bg0, hairline line at its bottom.
Status square 8×8 at x=12, vertically centered, color by state (4.4).
At x=28: state word in label, textLo (e.g. STREAM), then 8 dp gap, then title in body, textHi, ellipsis (stream title, or sarke when not streaming).
Right side, right to left: close, maximize/restore, minimize — each 46×32 dp. Then 8 dp gap, then text buttons FIT and HUD, each 40×32 dp, label style.
Caption glyphs, 10×10 dp, centered, textMid: minimize = 10×1 filled rect; maximize = hairline square; restore = two hairline squares offset 2 dp; close = two diagonals (antialiased, hairline stroke).
Hover: fill bg2, glyph textHi. Close hover: fill err, glyph textHi. Pressed: same as hover.
Text buttons: inactive textMid, active accent. HUD toggles the HUD. FIT toggles fit ↔ 1:1 (label changes to 1:1 when in 1:1 mode). In 1:1 the frame is centered and cropped, no scrolling.
4.3 Content area
Fill bg0. Fit mode: frame scaled to fit preserving aspect, centered; letterbox bars are bg0.
Input mapping: normalized coordinates are computed against the drawn image rect, not the client area. Mouse events over the letterbox are ignored and not sent. (Also agree with Temo whether the captured frame includes the title bar; mapping on A must match what is captured.)
Initial size before any stream: client 960×600 dp. On stream_start: frame size scaled to fit 80% of B's work area, + 32 dp strip, centered. Minimum client size 480×320 dp.
4.4 Connection states (no live frames)
State	Status square	Display word	Detail line (body, textMid)
Waiting (not connected)	textLo	WAITING	looking for host at {ip}:{port}
Connecting	warn, blinking	CONNECTING	connecting to {ip}:{port}
Linked, no stream	ok	LINKED	rtt {n} ms + second line: on the other pc press ctrl+alt+shift+w to send a window (hotkey from ARTEFACT "გადაწყვეტილებები 2", #5)
Streaming	accent	— (frames shown)	—
Stalled (streaming, no frame > 1000 ms)	warn	overlay STALLED	{t}s without frames (t with 1 decimal)
Lost (socket closed)	err	CONNECTION LOST	only if reconnect exists: reconnecting — otherwise restart the client

Layout of the state block (Waiting / Connecting / Linked / Lost), left-aligned at x=48, vertically centered in the content area:

SARKE (the app name) — label, textLo.
8 dp gap.
Display word — display, textHi, followed by 4 dp gap and a 12×28 dp accent block caret (blinking), top-aligned with the cap height area of the display line.
12 dp gap.
Detail line(s) — body, textMid, 4 dp between lines.

Footer inside content, x=48, bottom edge 24 dp above content bottom: {hostname} · {W}×{H} · {scale}% (B's own info from hello data), label, textLo.

Stalled: keep the last frame, draw bg0 at 60% alpha over the image, then centered STALLED (display, warn) and the detail line below (8 dp gap).

4.5 HUD (overlay on content, top-right)
Visible by default. Position: top = strip bottom + 12 dp, right = 12 dp from client right edge.
Fixed width 176 dp (never resizes with values). Padding 10 dp vertical, 12 dp horizontal. Row height 18 dp. Fill bg0 at 88% alpha, hairline line border.
Rows (label left in label/textLo; value right-aligned in readout/textHi, unit in label/textLo after one space):
Label	Value	Warn	Err
RTT	{n} ms from ping/pong	> 20	> 50
FPS	displayed frames/s, 1 s window	< 24	< 12
LAT	capture → display in ms using clock offset; — until offset known	> 80	> 150
MBPS	video payload, 1 decimal	—	—
RES	{W}×{H}	—	—
Q	JPEG quality from config	—	—
DROP	frames skipped (seq gaps) since stream start	—	—
Value color becomes warn/err per thresholds; otherwise textHi.
Update the HUD text at 4 Hz (every 250 ms), not every frame.
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
After the change: explain to the user (in Georgian, per CLAUDE.md) what was done and how to run the preview tool.
8. Do not
Do not add rounded corners, gradients (except the edge glow), shadows drawn by hand, glass/acrylic, or a second accent color.
Do not use GDI for text or shapes anywhere UI is drawn.
Do not use system EDIT/BUTTON controls in styled surfaces.
Do not animate anything not listed in 2.4.
Do not hardcode colors/sizes outside tokens.h.
Do not touch another developer's files; if your surface needs something from theirs, tell the user who to ask.

Future design changes will be made by editing docs/DESIGN.md and tokens.h only.

9. App logo, tray icon and notifications (added 2026-10-17, chosen by Sandro: concept C "Caret")
App name (Sandro, 2026-10-17): Sarke — Georgian სარკე, "mirror". Written "Sarke" (window title, tray, notifications), "SARKE" in label style (it uppercases), "sarke" as the viewer strip title when nothing streams. One place in code: tokens (kAppName, kAppNameIdle). The K mark below is from the KVM name and is being replaced; see the Sarke concepts.
Mark: a pixel letter K followed by a block caret, on a 16×16 grid, square tile, no rounding.
Tile fill bg0. K in textHi: stem (2,3) 2×10; joint (4,7) 2×2; upper arm (6,5) 2×2 and (8,3) 2×2; lower leg (6,9) 2×2 and (8,11) 2×2. Caret in accent: (11,5) 3×8. Coordinates are (x,y) w×h in grid units; the grid lives in tokens.h (logo::cells).
Rendering: drawn in code at the exact pixel size Windows asks for (SM_CXSMICON / SM_CXICON for the current DPI); grid cell edges are rounded to whole pixels (no antialiasing, no scaling of a bitmap). No .ico/.rc file is needed, so CMake does not change.
Uses: window icon of the viewer (small and big), tray icon, notification icon.
Tray icon: in the notification area ("show hidden icons"). Tooltip: {APP} · {STATE WORD}, e.g. Sarke · LINKED. Left click or double click opens the viewer; right click shows a menu: Show viewer, Quit.
Notifications: Windows draws them (bottom-right); we only provide icon, title and text. Connected: title "Connected to host", text "{ip}:{port}". Lost: title "Connection lost", text "reconnecting" (only if reconnect exists, otherwise "restart the client"). No notification for WAITING/CONNECTING.
