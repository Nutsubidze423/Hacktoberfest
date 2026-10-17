"""Functional tests for docs/preview/sarke-app.html (the Sarke app redesign prototype).

    python -m pip install playwright
    python docs/preview/test_sarke_app.py          # uses the installed Microsoft Edge, headless

Every check prints PASS/FAIL; the exit code is the number of failures. JavaScript errors on the
page count as failures too.
"""
import pathlib
import re
import sys

from playwright.sync_api import sync_playwright

URL = pathlib.Path(__file__).with_name("sarke-app.html").resolve().as_uri()
failures = []
js_errors = []


def check(name, ok, detail=""):
    print(("PASS  " if ok else "FAIL  ") + name + ("" if ok else f"   [{detail}]"))
    if not ok:
        failures.append(name)


def rect(page, sel):
    return page.locator(sel).first.bounding_box()


def text(page, sel):
    return page.locator(sel).first.inner_text().strip()


def toasts(page):
    return page.locator(".toast:not(.out)").count()


def open_page(browser, hash_="", reduced=False, size=(1400, 900)):
    ctx = browser.new_context(viewport={"width": size[0], "height": size[1]},
                              reduced_motion="reduce" if reduced else "no-preference")
    page = ctx.new_page()
    page.on("pageerror", lambda e: js_errors.append(str(e)))
    page.on("console", lambda m: js_errors.append(m.text) if m.type == "error" and "fonts.g" not in m.text
            and "jsdelivr" not in m.text and "Failed to load resource" not in m.text else None)
    page.goto(URL + hash_)
    page.wait_for_load_state("load")
    page.wait_for_timeout(150)
    return page


def state(page, st):
    page.click(f'#protoStates button[data-st="{st}"]')
    page.wait_for_timeout(50)


def nav(page, name):
    page.click(f'.nav .item[data-page="{name}"]')
    page.wait_for_timeout(50)


def test_states(browser):
    page = open_page(browser)
    check("starts LINKED on the Stream page", text(page, "#tbText") == "Linked · RTT 14 ms"
          and page.locator('.nav .item[aria-current="page"]').get_attribute("data-page") == "stream")
    check("LINKED hero shows the glow between the PCs", page.locator("[data-hero-map] [data-glow]").count() == 1)
    expected = {"waiting": ("Waiting for the other PC", "Waiting"), "connecting": ("Connecting", "Connecting"),
                "linked": ("Linked", "Linked · RTT 14 ms"), "lost": ("Connection lost", "Connection lost")}
    for st, (word, status) in expected.items():
        state(page, st)
        check(f"{st}: big word + title-bar status", text(page, "[data-word]") == word and text(page, "#tbText") == status,
              f"{text(page, '[data-word]')} / {text(page, '#tbText')}")
        check(f"{st}: only one state button pressed",
              page.locator('#protoStates button[aria-pressed="true"]').count() == 1)
    state(page, "connecting")
    check("connecting: status square blinks", "blinking" in page.locator("#tbSq").get_attribute("class"))
    state(page, "waiting")
    check("waiting: other PC drawn dimmed with '?'", "dim" in page.locator("[data-hero-map] [data-peer]").get_attribute("class")
          and text(page, "[data-hero-map] [data-peer] .num") == "?")
    check("waiting: no glow (nothing linked)", page.locator("[data-hero-map] [data-glow]").count() == 0)
    state(page, "stream")
    check("stream: picture + HUD shown", page.locator("[data-pic]").count() == 1 and page.locator("[data-hud]").count() == 1)
    check("stream: title-bar status", text(page, "#tbText") == "Streaming · 30 fps")
    state(page, "stalled")
    t1 = float(re.match(r"([\d.]+)s", text(page, "#stallT")).group(1))
    page.wait_for_timeout(600)
    t2 = float(re.match(r"([\d.]+)s", text(page, "#stallT")).group(1))
    check("stalled: timer starts near 1.2 s and counts up", 1.0 <= t1 < 2.0 and t2 > t1, f"{t1} -> {t2}")
    check("stalled: picture kept under the overlay", page.locator("[data-pic]").count() == 1 and page.locator(".stall").count() == 1)
    page.context.close()


def test_notifications(browser):
    page = open_page(browser)
    state(page, "waiting")
    check("linked -> waiting: no notification", toasts(page) == 0)
    state(page, "linked")
    check("waiting -> linked: 'Connected' card", toasts(page) == 1 and "Connected to tabuladze" in text(page, ".toast .t1")
          and page.get_attribute(".toast", "data-kind") == "connected")
    box, vw = rect(page, ".toast"), page.viewport_size["width"]
    page.wait_for_timeout(300)  # after the slide-in
    box = rect(page, ".toast")
    check("card sits at the top-right (16 px from the edges)", abs(vw - (box["x"] + box["width"]) - 16) <= 2
          and abs(box["y"] - 16) <= 2, f"{box} vw={vw}")
    state(page, "stream")
    check("linked -> stream: no extra notification", toasts(page) == 1 and page.get_attribute(".toast", "data-kind") == "connected")
    state(page, "lost")
    check("stream -> lost: the card becomes 'Connection lost' (one at a time)", toasts(page) == 1
          and page.get_attribute(".toast", "data-kind") == "lost" and text(page, ".toast .t1") == "Connection lost")
    state(page, "lost")
    check("lost -> lost (same): nothing new", toasts(page) == 1 and page.get_attribute(".toast", "data-kind") == "lost")
    page.hover(".toast")
    page.wait_for_timeout(5300)
    check("hover holds the card past its 5 s", toasts(page) == 1)
    page.click(".toast .x")
    page.wait_for_timeout(300)
    check("x closes it", toasts(page) == 0)
    nav(page, "settings")
    page.click("#capMin")
    state(page, "linked")
    page.click(".toast .t1")
    page.wait_for_timeout(300)
    check("clicking the card opens Sarke on the Stream page", page.is_visible("#stage") and toasts(page) == 0
          and page.locator('.nav .item[aria-current="page"]').get_attribute("data-page") == "stream")
    nav(page, "settings")
    page.click("#setNotifyUp")
    page.click("#setNotifyLost")
    check("notification switches turn off", page.get_attribute("#setNotifyUp", "aria-checked") == "false"
          and page.get_attribute("#setNotifyLost", "aria-checked") == "false")
    state(page, "waiting")
    state(page, "linked")
    state(page, "lost")
    check("switched off: no notifications", toasts(page) == 0)
    page.context.close()
    page = open_page(browser)
    state(page, "waiting")
    page.wait_for_timeout(50)
    state(page, "linked")
    page.wait_for_timeout(5600)
    check("the card goes by itself after 5 s", toasts(page) == 0)
    page.context.close()


def test_stream_page(browser):
    page = open_page(browser)
    # The bands only live 240 ms: count them in the same JavaScript turn as the click.
    bands = page.evaluate("""() => { document.querySelector('#protoStates button[data-st="stream"]').click();
                                     return document.querySelectorAll('[data-band]').length; }""")
    check("new stream: 8 'mirror' bands cover it", bands == 8, str(bands))
    page.wait_for_timeout(400)
    check("mirror reveal finishes (< 0.4 s)", page.locator("[data-band]").count() == 0)
    area, pic = rect(page, "#picArea"), rect(page, "[data-pic]")
    check("FIT: whole picture inside, 16:9", pic["width"] <= area["width"] + 1 and pic["height"] <= area["height"] + 1
          and abs(pic["width"] / pic["height"] - 16 / 9) < 0.02, f"{pic}")
    page.click('[data-cmd="fit"]')
    pic2 = rect(page, "[data-pic]")
    check("1:1: picture at its real size (1600 px)", text(page, '[data-cmd="fit"]') == "1:1"
          and abs(pic2["width"] - 1600 * (area["width"] / page.evaluate("document.getElementById('picArea').clientWidth"))) < 2)
    page.click('[data-cmd="fit"]')
    check("back to Fit", text(page, '[data-cmd="fit"]') == "Fit")
    page.click('[data-cmd="hud"]')
    check("HUD button hides the HUD", page.locator("[data-hud]").count() == 0)
    check("...and the Settings switch follows", page.get_attribute("#setHud", "aria-checked") == "false")
    nav(page, "settings")
    page.click("#setHud")
    nav(page, "stream")
    check("Settings switch turns the HUD back on", page.locator("[data-hud]").count() == 1)
    state(page, "stalled")
    check("stream -> stalled: no reveal (same stream)", page.locator("[data-band]").count() == 0)
    state(page, "stream")
    check("stalled -> stream: no reveal (same stream)", page.locator("[data-band]").count() == 0)
    n = toasts(page)
    page.click('[data-cmd="back"]')
    check("GIVE BACK: back to LINKED, no notification", text(page, "#tbText").startswith("Linked") and toasts(page) == n)
    page.context.close()
    page = open_page(browser, reduced=True)
    state(page, "stream")
    check("reduced motion: no reveal bands", page.locator("[data-band]").count() == 0)
    page.context.close()


def side_of(page):
    h, p = rect(page, '#arrMap [data-mon="1"]'), rect(page, "#arrMap [data-peer]")
    return h, p


def test_displays(browser):
    page = open_page(browser, "#linked-displays")
    check("displays: starts RIGHT, saved", text(page, "#rdEdge") == "Right edge" and text(page, "#rdLen") == "1080 px"
          and text(page, "#rdSide") == '"right"' and text(page, "#rdState") == "Saved")
    check("APPLY / REVERT disabled while nothing changed", page.is_disabled("#applyBtn") and page.is_disabled("#revertBtn"))
    check("TOP / BOTTOM locked until the host accepts them", page.is_disabled('#sideSeg button[data-side="top"]')
          and page.is_disabled('#sideSeg button[data-side="bottom"]') and page.is_visible("#sideNote"))
    check("status line says what the other PC needs", "on tabuladze, this PC must be on the left" in text(page, "#dispMsg"))
    h, p = side_of(page)
    check("right: other PC touches this PC's right edge", 0 <= p["x"] - (h["x"] + h["width"]) <= 6, f"{h} {p}")
    check("right: centred on the edge", abs((p["y"] + p["height"] / 2) - (h["y"] + h["height"] / 2)) <= 3, f"{h} {p}")
    page.click('#sideSeg button[data-side="left"]')
    h, p = side_of(page)
    check("left: placed on that edge", 0 <= h["x"] - (p["x"] + p["width"]) <= 6 and text(page, "#rdEdge") == "Left edge", f"{h} {p}")
    check("left: blue edge drawn", page.locator("#arrMap [data-glow] .glow").count() == 1)
    check("left: exactly one side button pressed", page.locator('#sideSeg button[aria-pressed="true"]').count() == 1)
    check("changed -> NOT SAVED, client_side \"left\", buttons enabled", text(page, "#rdState") == "Not saved"
          and text(page, "#rdSide") == '"left"' and not page.is_disabled("#applyBtn") and "Not saved yet" in text(page, "#dispMsg"))
    page.click('#sideSeg button[data-side="top"]', force=True)
    check("a locked TOP click changes nothing", text(page, "#rdEdge") == "Left edge")
    page.click("#revertBtn")
    check("REVERT -> back to saved RIGHT", text(page, "#rdEdge") == "Right edge" and text(page, "#rdState") == "Saved")
    page.click('#sideSeg button[data-side="left"]')
    nav(page, "stream")
    hm, hp = rect(page, '[data-hero-map] [data-mon="1"]'), rect(page, "[data-hero-map] [data-peer]")
    check("unsaved change NOT shown on the home map", hp["x"] > hm["x"] + hm["width"] - 1)
    nav(page, "displays")
    check("unsaved change kept when coming back", text(page, "#rdEdge") == "Left edge")
    page.click("#applyBtn")
    check("APPLY -> SAVED, says what to do on the other PC", text(page, "#rdState") == "Saved" and page.is_disabled("#applyBtn")
          and "Saved. Restart Sarke" in text(page, "#dispMsg") and "must be on the right" in text(page, "#dispMsg"))
    nav(page, "stream")
    hm, hp = rect(page, '[data-hero-map] [data-mon="1"]'), rect(page, "[data-hero-map] [data-peer]")
    check("home map shows the applied LEFT placement", hp["x"] + hp["width"] <= hm["x"] + 1)
    nav(page, "displays")
    page.click('#roleSeg button[data-role="client"]')
    check("client role: config 'left' means the host is on the RIGHT here", text(page, "#rdEdge") == "Right edge"
          and text(page, "#rdSide") == '"left"' and text(page, "#rdState") == "Saved")
    page.click('#sideSeg button[data-side="left"]')
    check("client role: host on the left -> client_side \"right\"", text(page, "#rdSide") == '"right"')
    page.click('#roleSeg button[data-role="host"]')
    check("back to host: unsaved edit dropped, LEFT as saved", text(page, "#rdEdge") == "Left edge" and text(page, "#rdState") == "Saved")
    page.context.close()


def drag_peer_to(page, dx, dy, release=True):
    p = rect(page, "#arrMap [data-peer]")
    x0, y0 = p["x"] + p["width"] / 2, p["y"] + p["height"] / 2
    page.mouse.move(x0, y0)
    page.mouse.down()
    page.mouse.move(x0 + dx / 2, y0 + dy / 2, steps=5)
    page.mouse.move(x0 + dx, y0 + dy, steps=5)
    if release:
        page.mouse.up()
    page.wait_for_timeout(50)


def test_drag(browser):
    page = open_page(browser, "#linked-displays")
    h = rect(page, '#arrMap [data-mon="1"]')
    p = rect(page, "#arrMap [data-peer]")
    drag_peer_to(page, (h["x"] - p["width"]) - p["x"] - 40, 0, release=False)
    check("while dragging: the drop zones show, LEFT lights up", page.locator("#arrMap [data-zone]").count() == 2
          and page.get_attribute('#arrMap [data-zone="left"]', "class") == "zone hot")
    check("while dragging: panel previews LEFT, NOT SAVED", text(page, "#rdEdge") == "Left edge" and text(page, "#rdState") == "Not saved")
    g, m1 = rect(page, "#arrMap [data-glow] .glow"), rect(page, '#arrMap [data-mon="1"]')
    check("while dragging: the blue edge moves to the left edge", 0 <= g["x"] - m1["x"] <= 6, f"{g} {m1}")
    page.mouse.up()
    page.wait_for_timeout(50)
    check("drop on the left -> LEFT, zones gone", text(page, "#rdEdge") == "Left edge" and page.locator("#arrMap [data-zone]").count() == 0)
    h, p = rect(page, '#arrMap [data-mon="1"]'), rect(page, "#arrMap [data-peer]")
    check("...snapped onto the left edge, centred", 0 <= h["x"] - (p["x"] + p["width"]) <= 6
          and abs((p["y"] + p["height"] / 2) - (h["y"] + h["height"] / 2)) <= 3, f"{h} {p}")
    # Above the right half: top isn't accepted yet, so it goes to the nearer of left / right.
    drag_peer_to(page, (h["x"] + h["width"] * 0.8) - (p["x"] + p["width"] / 2), (h["y"] - 60) - (p["y"] + p["height"] / 2))
    check("drop above the right half -> RIGHT (no TOP yet)", text(page, "#rdEdge") == "Right edge", text(page, "#rdEdge"))
    p0 = rect(page, "#arrMap [data-peer]")
    page.mouse.click(p0["x"] + 20, p0["y"] + 20)
    check("click without moving changes nothing", text(page, "#rdEdge") == "Right edge")
    page.focus("#arrMap [data-peer]")
    page.keyboard.press("ArrowLeft")
    check("keyboard: ArrowLeft -> LEFT, focus stays on the PC", text(page, "#rdEdge") == "Left edge"
          and page.evaluate("document.activeElement.hasAttribute('data-peer')"))
    page.keyboard.press("ArrowUp")
    check("keyboard: ArrowUp does nothing while TOP is locked", text(page, "#rdEdge") == "Left edge")
    page.keyboard.press("ArrowRight")
    check("keyboard: ArrowRight -> RIGHT", text(page, "#rdEdge") == "Right edge")
    # Regression: a drag that ends at a corner used to leave label text selected, and the NEXT drag
    # became the browser's own text drag (pointercancel) and was silently ignored.
    h, p = rect(page, '#arrMap [data-mon="1"]'), rect(page, "#arrMap [data-peer]")
    drag_peer_to(page, 0, 2000)
    h, p = rect(page, '#arrMap [data-mon="1"]'), rect(page, "#arrMap [data-peer]")
    drag_peer_to(page, (h["x"] - 80) - (p["x"] + p["width"] / 2), 0)
    check("regression: a second drag after one ending at a corner still works", text(page, "#rdEdge") == "Left edge",
          text(page, "#rdEdge"))
    check("no text gets selected by dragging", page.evaluate("String(getSelection())") == "")
    page.click("#revertBtn")
    page.click('#protoMons button[data-mons="2"]')
    check("2 monitors: both listed, right edge is the big monitor's 1440 px", page.locator(".monrow").count() == 2
          and text(page, "#rdLen") == "1440 px" and text(page, "#rdState") == "Saved", text(page, "#rdLen"))
    h2, p = rect(page, '#arrMap [data-mon="2"]'), rect(page, "#arrMap [data-peer]")
    check("2 monitors, right: next to monitor 2", 0 <= p["x"] - (h2["x"] + h2["width"]) <= 6, f"{h2} {p}")
    page.click('#sideSeg button[data-side="left"]')
    check("2 monitors, left: monitor 2's edges above / below monitor 1 count too (1080 + 120 + 240)",
          text(page, "#rdLen") == "1440 px" and page.locator("#arrMap [data-glow] .glow").count() == 3, text(page, "#rdLen"))
    page.context.close()


def test_detect(browser):
    ctx = browser.new_context(viewport={"width": 1400, "height": 900})
    try:
        ctx.grant_permissions(["window-management"])
    except Exception:
        pass  # older browsers: the page falls back to window.screen
    page = ctx.new_page()
    page.on("pageerror", lambda e: js_errors.append(str(e)))
    page.goto(URL + "#linked-displays")
    page.wait_for_timeout(150)
    check("before detecting: sample monitors", text(page, "#monSource") == "Sample" and "Sample" in text(page, "#arrMap .live"))
    page.click("#detectBtn")
    page.wait_for_timeout(300)
    real = page.evaluate("[Math.round(screen.width * devicePixelRatio), Math.round(screen.height * devicePixelRatio)]")
    rows = page.locator(".monrow").count()
    check("DETECT: uses this browser's real screen(s)", text(page, "#monSource").startswith("Detected") and rows >= 1
          and f"{real[0]}×{real[1]}" in text(page, ".monrow"), f"{text(page, '#monSource')} {rows} {real}")
    check("map says Live", "Live" in text(page, "#arrMap .live"))
    check("arrangement still works on detected screens", text(page, "#rdEdge") == "Right edge"
          and int(text(page, "#rdLen").split()[0]) == real[1] if rows == 1 else True)
    page.click("#identifyBtn")
    check("IDENTIFY: a big number on every monitor", page.locator("#arrMap .ident").count() == rows)
    page.wait_for_timeout(2700)
    check("IDENTIFY: gone after 2.5 s", page.locator("#arrMap .ident").count() == 0)
    page.click('#protoMons button[data-mons="1"]')
    check("prototype monitor buttons go back to samples", text(page, "#monSource") == "Sample")
    ctx.close()


def test_theme(browser):
    for scheme, want in (("dark", "rgb(5, 5, 6)"), ("light", "rgb(251, 251, 253)")):
        ctx = browser.new_context(viewport={"width": 1400, "height": 900}, color_scheme=scheme)
        page = ctx.new_page()
        page.on("pageerror", lambda e: js_errors.append(str(e)))
        page.goto(URL)
        page.wait_for_timeout(150)
        bg = page.evaluate("getComputedStyle(document.getElementById('app')).backgroundColor")
        check(f"system {scheme}: the app follows it (website palette)", bg == want, bg)
        other = "light" if scheme == "dark" else "dark"
        page.click(f'#protoTheme button[data-theme="{other}"]')
        check(f"forcing {other} switches the palette", page.evaluate("document.documentElement.dataset.theme") == other)
        ctx.close()


def test_settings_and_window(browser):
    page = open_page(browser, "#linked-settings")
    page.locator("#qRange").fill("85")
    check("quality slider updates its label", text(page, "#qVal") == "85")
    page.click('#fpsSeg button[data-fps="60"]')
    check("fps: one choice pressed", page.get_attribute('#fpsSeg button[data-fps="60"]', "aria-pressed") == "true"
          and page.locator('#fpsSeg button[aria-pressed="true"]').count() == 1)
    page.click('#page-settings [data-goto="displays"]')
    check("ARRANGE opens Displays", page.locator('.nav .item[aria-current="page"]').get_attribute("data-page") == "displays")
    page.click("#capMin")
    check("minimize: app hidden, tray note shown", page.is_hidden("#stage") and page.is_visible("#trayNote"))
    page.click("#reopen")
    check("OPEN SARKE: app back on the same page", page.is_visible("#stage") and page.is_visible("#arrMap [data-peer]"))
    page.click("#capClose")
    check("close: keeps running in the tray", page.is_visible("#trayNote"))
    page.click("#reopen")
    page.click("#capMax")
    w = rect(page, "#app")["width"]
    check("maximize: fills the page width", w > 1250 and page.get_attribute("#capMax", "aria-label") == "Restore", str(w))
    page.click("#capMax")
    check("restore: back to 1200 px", abs(rect(page, "#app")["width"] - 1200) < 2)
    page.set_viewport_size({"width": 900, "height": 800})
    page.wait_for_timeout(200)
    a = rect(page, "#app")
    check("narrow page: app scaled down to fit", a["width"] < 900, str(a))
    page.context.close()
    page = open_page(browser, "#stream-stream", size=(900, 800))
    area, pic = rect(page, "#picArea"), rect(page, "[data-pic]")
    check("scaled app: picture still fits its area", pic["width"] <= area["width"] + 1 and pic["height"] <= area["height"] + 1)
    page.set_viewport_size({"width": 1400, "height": 900})
    page.wait_for_timeout(250)
    area, pic = rect(page, "#picArea"), rect(page, "[data-pic]")
    check("after resize: picture re-fitted to the new size", abs(pic["width"] - area["width"]) < 2 or abs(pic["height"] - area["height"]) < 2,
          f"{area} {pic}")
    page.context.close()
    page = open_page(browser, "#lost-displays")
    check("deep link #lost-displays", text(page, "#tbText") == "Connection lost" and page.is_visible("#arrMap"))
    page.context.close()


with sync_playwright() as p:
    browser = p.chromium.launch(channel="msedge", headless=True)
    for t in (test_states, test_notifications, test_stream_page, test_displays, test_drag, test_detect,
              test_theme, test_settings_and_window):
        print(f"--- {t.__name__}")
        try:
            t(browser)
        except Exception as e:  # a crashed test is a failure, keep running the rest
            check(f"{t.__name__} ran without crashing", False, repr(e)[:300])
    browser.close()

check("no JavaScript errors on the page", not js_errors, "; ".join(js_errors[:5]))
print(f"\n{len(failures)} failure(s)")
sys.exit(len(failures))
