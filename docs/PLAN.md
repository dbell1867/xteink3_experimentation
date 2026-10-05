# Xteink X3 bring-up — plan

## Resume here
- **Position:** Stage 12 (lessons on the X3 as XTC) **done** — snapshot stages/12-xtc-lessons.
- **Pipeline:** `tools/mdpages.py docs/lesson-NN.md build/pages/X --xtc build/xtc/X.xtc` (landscape default,
  body 28 px, tables 25, code 24, symbol fallbacks) -> copy .xtc to /books -> X3 page viewer (page copied
  straight into the frame buffer). Check files with `tools/xtcinfo.py file.xtc pngdir` (bit-for-bit).
  Deps: tools/requirements.txt. Orientation: keys on the LEFT (Power right); lower page key = next.
- **Project goals:** see memory xteink3-project-goals (lessons on the bus; marimo talking points + two-way clicker).
- **Next (pick):** USB-serial sync of .xtc files (no card swaps); marimo test deck + speaker-notes export;
  spike: can a laptop helper reach reveal.js inside marimo's page?
- **Git:** commit per stage locally; the USER pushes (never push — see memory).
- **Workflow notes:** stop any follow_serial capture before uploading (it holds the port); don't `pkill -f` a pattern
  that's in your own shell's command line. When copying to SD: /books at card ROOT, and eject before removing.

## Done
1. MCU identified: ESP32-C3, 16 MB flash, no PSRAM (esptool).
2. Factory firmware backed up + verified → backup/ (restore command in backup/README.md).
3. Pin map researched → docs/hardware-x3.md.
4. PlatformIO project scaffolded (pioarduino 55.03.311, same as CrossPoint Reader).
5. Stage 1 probe: I2C = 0x55/0x68/0x6B, display = UC8253, buttons measured (docs/data/buttons-x3.csv).
   Bug hit: qio flash mode boot loop -> fixed with board_build.flash_mode = dio.
6. Stage 2 first light: own minimal UC8253 driver, OEM FULL LUTs; refresh 925 ms (predicted 1-3 s).
   Portrait = setRotation(3); bezel hides 3 px each side. Snapshots: stages/02-first-light, stages/02b-portrait-bezel.
7. Stage 3 buttons: all 7 mapped, 0 glitches (predicted few/none), long press at 700 ms, presses during refresh counted.
   Power held 5.2 s does not power off / reset.
8. Stage 4 page turns: FAST 467 ms (predicted 400-450), NORMAL 744 ms & messier (prediction "NORMAL cleaner" failed),
   HALF auto-upgrade after idle works. FAST chosen.
9. Stage 5 ghosting: 20 FAST turns = ghost 2/3, HALF -> 1, RESYNC (2259 ms) -> 0. All three predictions held.
   Default: RESYNC every 15 turns.
10. Stage 6 SD + text reader: SD shares SPI, 25 MHz, 1.5 MB/s. Alice -> 156 pages (exact match with host sim),
    index 596 ms, quotes/dashes mapped to ASCII, resume via NVS works. Detour: Raspberry Pi card inserted by mistake,
    then /books not flushed / misplaced -> added directory logging to findBook().
11. Stage 7 battery + power-off: GPIO13 = battery latch (prediction held: battery power-off -> POWERON boot #1);
    USB -> deep sleep, wake on Power (reset 8, wake 7). Awake draw 21 mA (gauge). Wake-to-page 3.6 s.
12. Stage 7b fast wake: measured first (timeline in NVS), then 3 fixes -> battery wake 5.08 -> 1.84 s (predicted ~1.8).
    Surprise: SD cold start 648 ms after battery power-off (GPIO13 also cuts SD power) vs 47 ms warm.
13. Stage 8 light sleep: power bench on the gauge. 80 MHz saves little (pred. failed), light sleep 50 ms = 2.0 mA (10x),
    panel rails on costs ~0 (pred. failed). Reader light-sleeps when idle; no felt latency, no missed presses.
14. Stage 9 typography: cp1252 Noto Serif via own fontconvert.py (GFX is 8-bit). Real quotes/dashes/accents.
    21 px too small (my mistake: matched old pixel size, ignored 257 ppi) -> selectable 28-40 px, 28 chosen.
    Page counts matched host sim exactly at every size. Licence check caught Arch's stale Apache file -> real OFL.
15. Stage 10 navigation: menu on tap Back, contents (chapter detection), book picker, per-book positions, chapter
    page breaks + centred headings. Alice 276 p / 12 ch matched pagesim.py exactly.
16. Stage 11 faster wake: hypothesis "25 MHz fails cold" falsified (card's own 648 ms init). Saved frame + refresh
    overlapping SD: visible 1.82 -> 0.83 s; settle via BUSY ISR: ready 1.30 -> 1.10 s (predicted 1.10).
    Two user tests didn't reach the new code (ran before flash / no unplug) — confirm via kernel USB log + NVS markers.
17. Housekeeping: 5 lesson docs; .clangd for the C3 (thousands of clangd errors -> 0); skill updated:
    G55 (QIO/DIO boot loop), G43 RISC-V variant, G42 X3 example, template note.
18. Stage 12 XTC lessons: PC renders Markdown -> 1-bit landscape pages (same FreeType mono as the device fonts),
    exports Xteink XTC (format from CrossPoint); independent checker: 0 mismatches over 67 pages. X3 viewer + menu
    orientation setting; tables readable at 25 px; keys-left preferred, lower key = next.

## Decisions
- C++ / Arduino core 3.x via pioarduino — stock fw is also Arduino-ESP32, CrossPoint proves the platform on this device.
- Own pin header (include/board_pins.h) rather than depending on freeink-sdk — learn by reading its code instead.
- Display driver choice deferred until the controller probe says UC8253 or UC8279d.

## Open questions
- (PARKED 2026-10-05) Long-press on page keys fires the 1-page turn first, then the +10 jump (jarring). Options discussed 2026-10-05:
  A act-on-release (clean, but +~190 ms per turn: measured taps 130-240 ms), B press + auto-repeat for page keys,
  C separate jump gesture, D delayed refresh (rejected). Leaning: per-key policy (page keys B, Back/Power A).
  Decision: keep as-is for now; revisit (option: side-by-side A/B test on device).
- USB drops were the magnetic cable; unplug does NOT reset (measured). One reboot at 10:35:49 unexplained — boot counter now in fw.
- NORMAL mode: 2 short waveforms (175/368 ms) on rapid re-press; how does stock fw drive the OEM normal LUTs?
- Resolution cmd says 792x600 but panel is 528 rows — why does it work?
- (answered) -20 mA on USB at 100 % = charge terminated, battery feeds board; only USB hint is gauge current sign.
