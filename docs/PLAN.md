# Xteink X3 bring-up — plan

## Resume here
- **Position (2026-10-06):** Stage 16b **done** — snapshot stages/16-x3-library. Plug the X3 in -> it boots
  (5/6) -> udev starts the x3-sync user service -> changed lessons/talks rendered + sent -> Omarchy notification.
  16c done: marimo manager ~/Work/Python/Marimo/x3_manager (own local repo, no remote yet):
  `uv run marimo run app.py`. Nothing planned next - pick from real use.
- **Adding sources:** `x3 add <project>/docs --include 'lesson-*.md'` / `x3 add <deck folder>`; `x3 status`;
  `x3 rm` is the only delete. **Before flashing firmware: `x3 auto off`** (each upload re-enumerates -> a sync
  grabs the port), `x3 auto on` after. Log: `journalctl --user -u x3-sync`.
- **Seamless-sync plan (agreed 2026-10-06):** cable only (no Wi-Fi creds on the X3, no Windows: work decks get sent
  to this PC). Library root ~/X3: lessons/<series> and talks/<deck> = SYMLINKS (no copies) to project docs/ and
  deck repos (each deck its own repo). 16 = `x3` command via uv tool (scan ~/X3, find X3 by USB ID 303a:1001,
  render changed, send, NEVER delete automatically; add/status/sync/rm) + X3 Books folders per series + sync
  date. 16b = auto-sync on plug-in (udev -> systemd user service, notify, port lock). 16c = optional marimo
  manager (status, add via file browser, previews, confirmed delete) on the same module.
  Both goals work: lessons on the bus (6 lessons on the X3) and marimo talking points + Bluetooth clicker.
  X3 runs the normal build (30-min clicker auto-off).
- **Presenting:** X3 menu -> "Clicker: off" -> Confirm (radio on; laptop reconnects by itself once paired as
  "X3 Clicker"). Open talk-<deck>.xtc. Lower key = Page Down to the laptop, then the notes page; upper = Page Up;
  tap Confirm = resync laptop to the X3 (Home + n x Page Down); Contents jump resyncs too.
  Turning the clicker off (menu, or 30 min without a key) RESTARTS the X3 (~1.1 s back to the page via the
  saved-frame path) to drop the BLE stack. Tested with a 60 s build: no crash, no leak, laptop disconnects. Uses ~45 mA connected / 49 advertising (gauge, on battery).
- **Talk / lesson workflow:** edit (marimo speaker notes, or docs/*.md) -> plug in the X3. That's it.
- **Next (pick):** two-way slide helper only if one-way drift hurts in real talks. (Long-press redesign DROPPED 2026-10-06.)
- **Done today:** lesson 06 (stages 12-15b) on the X3; skill updated to G62 (G56 BLE HID + on/off life cycle,
  G57-G61, G62 SD-open power; G34/G35 deinit advice corrected).
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
19. Stage 13 USB sync: @-line protocol + 4 KB acked chunks, .part + CRC then rename; ROM CRC32 == zlib (checked).
    Throughput 19 KB/s at first (PC read(256) waited out its 0.2 s timeout per ACK) -> 283 KB/s (predicted 150-300).
    Bad-CRC PUT refused, nothing left behind. Defaults made project-relative after spotting a --delete hazard.
20. Stage 14 talking points: marimo 0.25.1 slides layout format read from marimo's own code (notes per cell,
    by position). tools/notes2xtc.py: one page per step, titles via AST (regex failed twice). Bug: XTC names are
    UTF-8 but the UI font is cp1252 ("Â·") -> shared src/cp1252.h. Compact talk layout: no overflow.
21. Stage 15 clicker: X3 = BLE HID keyboard (core BLE lib, NimBLE, Just Works bonding). BLE heap 82 KB, flash +320 KB.
    Page Down steps reveal.js linearly (13 x from Home landed exactly on step 14). First "failure" = X3 and laptop
    not starting aligned; resync fixes. Menu status was stale (no redraw on state change) -> auto-redraw + logs.
    Power: predicted 29-30 mA, measured 45 connected / 49 advertising (on battery; the USB reading was the charger,
    G50). Hypothesis "short HID connection interval" FALSIFIED (advertising costs as much): radio-on ~+25 mA.
22. Stage 15b auto-off test (60 s test build, -DTEST_CLICKER_IDLE_MS=60000): found 3 bugs.
    (1) stop() = deinit(true) while connected -> crash (teardown events hit freed characteristics) -> disconnect,
    wait, stop advertising, then deinit. (2) deinit(true) releases controller memory for good -> next start crashed
    in esp_bt_controller_init. (3) deinit(false) leaks ~7.6 KB/cycle (BLEServer has no destructor). Fix: build once;
    off = disconnect + stop advertising + esp_restart() with a softRestart flag -> saved-frame fast wake (1.08 s).
    Bench then showed 18.6 mA in light sleep - blamed on BLE WITHOUT a control; a control run with Alice open read
    2.0, with the talk .xtc open 18.6: XtcBook kept the file open after a 52 KB multi-block read -> SD card stayed
    active ~17 mA. Fix: open/read/close per page (as CrossPoint does) -> 2.0 mA with the talk open.
    The restart decision rested on the confounded 19 mA; kept anyway (robust, returns 82 KB). "Build once, no
    restart" idle power is UNMEASURED.
23. Stage 15c "build once, no restart" measured (test build -DTEST_CLICKER_NO_RESTART: off = stop only).
    Predicted 2-4 mA, light sleep accepted. Measured: accepted (rej 0, full 50 ms sleeps) but 18.9 mA
    (100 ms: 18.4; +panel 19.0) vs 2.0 control (same talk open, BLE never built) -> prediction FAILED.
    Also: after light sleep the clicker did NOT reconnect; control off -> on with USB plugged (no light
    sleep) reconnected and sent keys. Core has CONFIG_BT_CTRL_MODEM_SLEEP off. Restart on "off" kept, now on
    two measured reasons. (Capture lost the control run: the X3 re-enumerated as /dev/ttyACM1.)
24. Stage 16a: plug USB into a powered-off X3 (battery latch released, "Sleeping" banner) -> it boots by itself:
    banner replaced by the page, kernel enumerates 303a:1001. Done twice (14:44:43, 14:45:07). Predicted ~60%.
    Reset reason not read (talk page has no header).
25. Stage 16: `x3` command (tools/x3.py, uv tool install --editable .): library ~/X3 of symlinks, finds the X3
    by USB id + @HELLO, render cache ~/.cache/x3 (renders byte-identical to the old tool), sends what differs,
    never deletes. X3: sync protocol 2 (folder/file, @DATE), Books folders + "synced <date>". First sync 7 files
    5.2 MB in 18.7 s (predicted ~19). 7 old flat duplicates removed with x3 rm (user's call).
    16a re-test: one plug-in did NOT boot (15:05, cable fine: Power press enumerated at once); then 3 more
    booted (after 13 min off, 10 s off, idle auto-off). "Leftover charge" theory falsified; miss unexplained.
26. Stage 16b auto-sync: udev rule (this X3's serial) -> SYSTEMD_USER_WANTS x3-sync.service (oneshot).
    Plug-in -> lesson 06 sent 12 s later. Bug: notify-send exit 0 but nothing shown on Omarchy -> use
    omarchy-notification-send first (shown). Skill G63.
27. Stage 16c marimo manager (user chose to build it anyway): status table (+ Check the X3, Sync now), page
    previews (match the X3), delete with confirmation (+ warning if the library would put it back), sources
    add (file browser) / remove (symlink only). Uses x3.py as a library (X3Error API, lock released per
    session); marimo pinned 0.25.1. UX bugs from the user's test: Preview had no heading and was missed;
    the delete refusal showed in the top log, far from the button -> messages next to their buttons.

## Decisions
- C++ / Arduino core 3.x via pioarduino — stock fw is also Arduino-ESP32, CrossPoint proves the platform on this device.
- Own pin header (include/board_pins.h) rather than depending on freeink-sdk — learn by reading its code instead.
- Display driver choice deferred until the controller probe says UC8253 or UC8279d.

## Open questions
- (DROPPED 2026-10-06 by the user; behaviour stays as-is) Long-press on page keys fires the 1-page turn first, then the +10 jump (jarring). Options discussed 2026-10-05:
  A act-on-release (clean, but +~190 ms per turn: measured taps 130-240 ms), B press + auto-repeat for page keys,
  C separate jump gesture, D delayed refresh (rejected). Leaning: per-key policy (page keys B, Back/Power A).
  Decision: keep as-is for now; revisit (option: side-by-side A/B test on device).
- USB drops were the magnetic cable; unplug does NOT reset (measured). One reboot at 10:35:49 unexplained — boot counter now in fw.
- NORMAL mode: 2 short waveforms (175/368 ms) on rapid re-press; how does stock fw drive the OEM normal LUTs?
- Plug-in boot: 5 of 6. One miss (15:05) unexplained - connector seating? Watch for it.
- @LIST computes every file's CRC each sync (~1.1 MB/s card read: 5.3 s for 5.2 MB). If the library grows,
  store CRCs at PUT time.
- Not additive: BLE resident 18.9 mA, open .xtc 18.6, both together ~18.6-19. Same ~17 mA load kept awake by
  either, or coincidence? (Would need both fixed/unfixed in one bench session to tell.)
- Resolution cmd says 792x600 but panel is 528 rows — why does it work?
- (answered) -20 mA on USB at 100 % = charge terminated, battery feeds board; only USB hint is gauge current sign.
