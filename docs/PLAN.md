# Xteink X3 bring-up — plan

## Resume here
- **Position:** Stage 7b (fast wake) **done** — snapshot stages/07b-fast-wake. Battery wake 5.08 s -> 1.84 s.
- **src/:** main.cpp = reader + battery + power-off/deep sleep + wake timeline (NVS "wake", printed next boot);
  text_book has index cache (<book>.idx, validated by book size + layout signature; bump IDX_MAGIC if tokenizer
  rules change); driver allows HALF without an old frame. NVS "cleanSleep" flag -> HALF on wake.
- **Possible further wake gains (~0.5 s total, diminishing):** overlap SD cold start with display init,
  save sleep frame + FAST (CrossPoint way), skip RAM-plane clears on HALF wake.
- **Next (pick):** light sleep between turns (21 mA awake), font with real quotes/accents, chapters + book
  picker, or housekeeping (G55, G7, lesson docs).
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

## Decisions
- C++ / Arduino core 3.x via pioarduino — stock fw is also Arduino-ESP32, CrossPoint proves the platform on this device.
- Own pin header (include/board_pins.h) rather than depending on freeink-sdk — learn by reading its code instead.
- Display driver choice deferred until the controller probe says UC8253 or UC8279d.

## Open questions
- Long-press on page keys fires the 1-page turn first, then the +10 jump (jarring). Options discussed 2026-10-05:
  A act-on-release (clean, but +~190 ms per turn: measured taps 130-240 ms), B press + auto-repeat for page keys,
  C separate jump gesture, D delayed refresh (rejected). Leaning: per-key policy (page keys B, Back/Power A).
  Decision: keep as-is for now; revisit (option: side-by-side A/B test on device).
- USB drops were the magnetic cable; unplug does NOT reset (measured). One reboot at 10:35:49 unexplained — boot counter now in fw.
- NORMAL mode: 2 short waveforms (175/368 ms) on rapid re-press; how does stock fw drive the OEM normal LUTs?
- Resolution cmd says 792x600 but panel is 528 rows — why does it work?
- (answered) -20 mA on USB at 100 % = charge terminated, battery feeds board; only USB hint is gauge current sign.
