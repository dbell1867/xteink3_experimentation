# Lesson 06 — Lessons on the bus, talking points, and a Bluetooth clicker

**Stages:** 12–15c · **Date:** 2026-10-05/06
**Code:** `stages/12-xtc-lessons/` … `stages/15b-clicker-off/` · **PC tools:** `tools/mdpages.py`, `xtcinfo.py`,
`x3sync.py`, `notes2xtc.py` · **Device:** `src/xtc_book.*`, `sync_server.*`, `clicker.*`, `cp1252.h`

## Objectives
- Read my own Markdown lessons on the X3 — with readable tables and code.
- Update them over the USB cable, no card swaps.
- Show a marimo deck's speaker notes on the X3, one page per click.
- Make the X3's keys change the slides on my laptop.

## The big decision: render on the PC
The X3's text engine reads plain `.txt` in one font. Lessons have tables, code, bold and
symbols. Three options were weighed: parse Markdown on the X3, pre-process on the PC into a
simple format, or **render finished pages on the PC**. With the font size already settled,
on-device reflow wasn't needed — so the PC renders, the X3 just shows pages.

| Kept from earlier stages | New |
|---|---|
| FreeType hinted 1-bit glyphs (`fontconvert.py`) | Markdown parser (markdown-it-py) |
| the layout ideas from `pagesim.py` | tables, code, headings, symbol fallbacks |
| Noto Serif 28 px body | Noto Sans 25 px tables, Noto Sans Mono 24 px code |

Because the PC uses the **same** glyph rendering as the device fonts, the PNG previews *are*
what the X3 shows — not an approximation.

## Stage 12 — lessons as XTC pages
- **Landscape for all lessons** (chosen by eye): 792 px wide fits real table columns; portrait
  squeezed them into 1–2 words per line.
- A landscape page (792×528) is the **panel's native layout**, so the X3 copies a page from
  the SD card **straight into the frame buffer** — no decoding.
- Container: **Xteink's XTC format** (header, title, chapters, page table, 1-bit pages),
  learned from CrossPoint's parser. `##` sections become chapters → Contents works.
- `xtcinfo.py` is an **independent reader** (written from CrossPoint's parser, not from my
  writer): 0 mismatches over all 67 pages, bit for bit against the PNGs.
- Orientation is a menu setting; **keys on the left** felt right, and the **lower key is always
  "next"** (the rotation turns the key column upside down, so the mapping flips with it).

## Stage 13 — USB sync
`x3sync.py` re-renders changed lessons and sends only files whose size + CRC32 differ, over
the same USB serial link as the logs:
- protocol lines start with `@`; everything else on the link is log output and is ignored;
- files arrive in 4 KB chunks, each acknowledged, into `name.part`; the CRC must match before
  it's renamed into place — an interrupted transfer never replaces a good book;
- the X3's ROM CRC32 and Python's `zlib.crc32` were checked to agree before trusting "unchanged".

## Stage 14 — talking points from marimo
- marimo 0.25.1 stores slide settings in `layouts/<deck>.slides.json`: one entry per cell,
  **matched to cells by position** (read from marimo's own code, not guessed).
- `notes2xtc.py` builds reveal.js's **step order** — each slide, each fragment, each vertical
  sub-slide — and renders one X3 page per step: title, "step 2/4", notes, "Next ▸".
- Slide titles come from the cells' **string values via Python's `ast`** — two regex attempts
  failed (an apostrophe, a heading inside `mo.vstack(...)`).

## Stage 15 — the X3 as a Bluetooth keyboard
- The C3 is BLE-only, so it's a **BLE HID keyboard** ("Just Works" bonding, like a commercial
  clicker) — built on the core's BLE library, no extra dependency.
- **Page Down / Page Up** follow reveal.js's *linear* order (fragments and sub-slides);
  Right/Left skip vertical sub-slides.
- Keys go to the laptop **first**, then the notes page refreshes (~0.5 s later).
- **Resync** (tap Confirm, or any Contents jump): Home, then one Page Down per step — the
  laptop is made to match the X3.

## Stage 15b — switching the clicker off (and an SD power bug)
A forgotten clicker must turn itself off (30 min without a key). Testing that with a **60 s
test build** (`PLATFORMIO_BUILD_FLAGS=-DTEST_CLICKER_IDLE_MS=60000`) found four bugs:
1. **Crash on switch-off while connected.** `deinit(true)` freed the BLE objects, then the
   link's teardown event reached them. Fix: disconnect, wait for it (10–40 ms), then stop.
2. **Crash on switching back on.** `deinit(true)` releases the controller's memory for good —
   the library says so in a comment.
3. **A leak of ~7.6 KB per on/off cycle** with `deinit(false)`: the server's services are
   never freed. Fix for 1–3: build the BLE stack **once**; "off" = disconnect, stop
   advertising, **restart** — the saved-frame fast wake has the page back in **1.08 s**.
4. **18.6 mA idle instead of 2.0** — and the first explanation (the resident BLE stack) was
   wrong. The bench's new sleep counters showed the CPU *was* sleeping; a control run with a
   `.txt` book open read 2.0 mA. Cause: the `.xtc` file stayed open after a 52 KB read, so the
   **SD card stayed active**. Fix: open, read, close per page (as CrossPoint does).

## Stage 15c — could "off" skip the restart?
Bug 4's fix left a question: the restart was chosen while the 18.6 mA was still blamed on
Bluetooth. So a test build (`-DTEST_CLICKER_NO_RESTART`) made "off" only stop the radio, and
the bench ran with the stack still in memory — same talk open as the 2.0 mA control.
- Light sleep was **accepted** (0 rejected, full 50 ms sleeps) but drew **18.9 mA**.
- After that sleep the clicker **didn't reconnect**. Control: off → on with USB plugged in
  (no light sleep) reconnected fine — so it's sleep, not stop/start, that breaks it.
- The core is built with Bluetooth controller sleep off (`CONFIG_BT_CTRL_MODEM_SLEEP`).
The restart stays — now on two measured reasons instead of one confounded one.

Crash addresses were decoded with `riscv32-esp-elf-addr2line` on the exact firmware flashed.

## Predictions: held and failed
| Prediction | Result |
|---|---|
| Sync 150–300 KB/s | first 19 KB/s (a PC-side bug), then **283** ✓ |
| A focused slider steals the arrow keys | ✗ — slides still changed |
| Page Down steps through fragments + sub-slides | ✓ — 13 presses from Home landed exactly on step 14 |
| Clicker ~29–30 mA | ✗ — **45 connected, 49 advertising** |
| Cost is the short HID connection interval | ✗ — advertising costs as much: radio-on ≈ +25 mA |
| 19 mA idle comes from the resident BLE stack | ✗ — it was the open `.xtc` file (control: `.txt` = 2.0 mA) |
| Per-page open/close brings it back to ~2 mA | ✓ — 2.0 mA with a talk open |
| Stack kept but stopped: light sleep ~2–4 mA | ✗ — **18.9 mA**, and no reconnect after sleep |

## Real bugs and fixes
| Symptom | Cause | Fix |
|---|---|---|
| Sync at 19 KB/s | PC `read(256)` waited its full 0.2 s timeout for each 10-byte ACK | read what has arrived (`in_waiting`) |
| `--delete` from the wrong folder would empty the X3 | default paths relative to the current folder | paths from the project; refuse an empty set |
| "Â·" in menus; "â€”" for dashes | XTC names are UTF-8, the UI font is cp1252 | shared `cp1252.h` converter |
| Headings ran off the page | wrapped with the body font, drawn at heading size | measure with the heading's own font |
| Overflow after "keep heading with next" | moved rows without re-checking the page | decide before placing |
| ✓ ✗ ↪ drawn as boxes | not in Noto Serif/Sans | fallback chain: Noto Sans Math / Symbols |
| Menu said "waiting" while connected | menu only redrew on input | redraw when the clicker's state changes |
| First clicker run "broke" | X3 and laptop not on the same step | resync; start aligned |
| Battery read −29 mA (charging!) on USB | the gauge sees the charger (G50) | measure on battery |
| Crash switching the clicker off | `deinit(true)` while a laptop was connected | disconnect first, then stop |
| Crash switching it back on | `deinit(true)` released the controller memory | build once; restart for "off" |
| Free heap −7.6 KB per on/off | `deinit(false)`: the library never frees the server's services | build once; restart for "off" |
| 18.6 mA in light sleep | `.xtc` file left open after a 52 KB read kept the SD card active | open/read/close per page |
| Upload "port doesn't exist" | my wait loop ran the upload after timing out | stop on timeout |

## Python for the PC side (and the C++ that met it)
- `ast.parse(src)` + `ast.walk` gives a notebook's **string values** with quotes resolved —
  the reliable way to read code, unlike regex on source text.
- `struct.pack("<IBBH…", …)` writes a binary header byte-exactly — the Python twin of the
  C++ `#pragma pack` structs it must match.
- `PIL.Image.tobytes()` on a mode-"1" image is already "8 pixels per byte, MSB first,
  1 = white" — exactly the XTC page and our frame buffer.
- C++: `volatile State st` — written by the BLE task, read by `loop()`; `volatile` stops the
  compiler caching it.

## Cheat-sheet
```bash
.venv/bin/python tools/mdpages.py docs/lesson-06-*.md build/pages/x --xtc build/xtc/x.xtc  # one lesson
.venv/bin/python tools/xtcinfo.py build/xtc/x.xtc build/pages/x                           # verify it
.venv/bin/python tools/x3sync.py              # render changed lessons + send (X3 awake, plugged in)
.venv/bin/python tools/notes2xtc.py ~/Work/Python/Marimo/xteink3_deck/deck.py             # talk pages
cd ~/Work/Python/Marimo/xteink3_deck && uv run marimo run deck.py   # present; S = notes window
```
On the X3: Back → Clicker → Confirm; lower key = next; tap Confirm = resync; Clicker again = off (restarts).
```bash
PLATFORMIO_BUILD_FLAGS=-DTEST_CLICKER_IDLE_MS=60000 pio run -t upload   # 60 s auto-off test build
~/.platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-addr2line -pfiaC \
    -e .pio/build/xteink_x3/firmware.elf 0x4200... 0x4200...           # decode a crash
```

## Glossary
- **XTC** — Xteink's book format: pre-rendered pages plus a page table and chapters.
- **HID over GATT** — a Bluetooth LE keyboard/mouse; laptops support it natively.
- **Bonding** — pairing that's remembered, so the laptop reconnects by itself.
- **Fragment** — a slide item revealed by its own click; **sub-slide** — a slide below another (Down arrow).
- **Resync** — make the laptop's slideshow match the step shown on the X3.
- **Control measurement** — the same test with only the suspect removed; it's what found the SD bug.
