# Xteink X3 — hardware reference

Status legend: **measured** = confirmed on this unit; **source** = from community firmware
(CrossPoint Reader / freeink-sdk `BoardConfig.h`, reverse-engineered from stock fw); **todo** = still to verify (G27).

## Flash mode — MUST be DIO
`board_build.flash_mode = dio`. The generic C3 board defaults to qio → bootloader puts the Puya flash in quad mode →
reads return 0xFF → boot loop `partition 0 invalid magic number 0xffff` (even though esptool verified the write).

## Core (measured, esptool 2026-10-05)
- ESP32-C3 QFN32 rev v0.4, single-core RISC-V 160 MHz, Wi-Fi + BLE 5, 40 MHz crystal
- 16 MB Puya flash (0x85/0x2018), **no PSRAM** (~400 KB SRAM total)
- Native USB-Serial/JTAG (303a:1001) → /dev/ttyACM0
- Stock fw: Arduino-ESP32 3.x (IDF 5.5.4); partitions in `backup/README.md`

## Display (source)
- 792 × 528 mono e-paper, SPI @ 10 MHz
- Controller: **UC8253** (early run) **or UC8279d** (newer run) — same pins. Tell apart by reading
  UC81xx cmd 0x70 (VER) / 0x71 (FLG) over the half-duplex MOSI line.
- **measured: this unit = UC8253** — FLG=0x13 (real, driven status → read path works), VER=FF×5 (no VER register), both passes, BUSY idle=1
| Signal | GPIO |
|---|---|
| SCLK | 8 |
| MOSI (also half-duplex read) | 10 |
| CS | 21 |
| DC | 4 |
| RST | 5 |
| BUSY | 6 |

### Display — measured on this unit (stage 2, 2026-10-05)
- FULL refresh (OEM LUTs, verified byte-for-byte in factory fw @ 0x79485): **refresh 925 ms**, power-on 125 ms,
  power-off 52 ms, LUT+image upload 90 ms at 10 MHz, controller init 169 ms. Total displayFull ≈ 1.19 s.
- BUSY is two-phase on every command: drops LOW (working) then returns HIGH (done).
- Frame is sent bottom row first ("flipped"); 1 bit = white. 1-px stripes resolve cleanly.
- **Orientation:** GFX `setRotation(3)` = portrait with the power button at the top.
- **Bezel hides 3 px on every side** (ruler marks at 4 px fully visible, 2 px a sliver) → visible ≈ 522×786 portrait.
- Resolution cmd sends 792×600 (0x0318, 0x0258) yet the 528-row frame lands correctly — still unexplained.

### Refresh modes — measured (stage 4)
| Mode | Total | Waveform | Notes |
|---|---|---|---|
| FAST (community turbo LUT, CDI 0x29) | **467 ms** | 378 ms | page-turn mode; no flash; looked cleaner |
| NORMAL (OEM 'normal' LUT via SDK conditioning-pass setup, CDI 0xA9 + partial window) | 744 ms | 455 ms | looked **messier** than FAST (prediction failed); 2/20 turns had short waveforms (175, 368 ms) on rapid re-press — unexplained |
| HALF (scrub) | 870 ms | 454 ms | incl. 127 ms power-on; auto-used when rails were off |
| FULL | 1727 ms (2383 first after boot) | 925 ms | + no-op FAST settle (+ first-time conditioning pass) |
| RESYNC (FULL + OEM 'normal' conditioning pass + FAST settle) | 2259 ms | 926 ms | CrossPoint's X3 periodic clean-up |
Rails kept on between turns (saves ~127 ms power-on); powered off after 30 s idle.

### Ghosting — measured by eye (stage 5, rating 0 none … 3 obvious)
After 20 FAST turns: **2**. HALF clean-up: **1**. RESYNC: **0**. Data: docs/data/ghosting-x3.csv.
→ Default policy: FAST turns + RESYNC every 15 turns (CrossPoint default; its X3 "HALF" = requestResync(1)).
How Xteink's stock fw drives its 'normal' LUTs is unknown — the SDK only uses them for conditioning.

## Power — measured (stage 7)
- **GPIO13 is the battery latch** (and SD power): HIGH keeps the board on; LOW on battery = power cut.
  Proof: power-off on battery then Power key → reset reason 1 (POWER-ON), RTC boot counter wiped (boot #1).
  Firmware must drive GPIO13 HIGH as its very first action (Power-key boot from battery).
- On USB, the same sequence is a real deep sleep: wake on Power key (GPIO3 LOW, esp_deep_sleep_enable_gpio_wakeup)
  → reset reason 8 (DEEPSLEEP), wake cause 7 (GPIO), counter keeps counting.
- Panel parked with POWER_OFF + DEEP_SLEEP (0x07, 0xA5) before cutting power; the page stays on the glass.
- Wake (Power press) → page: **5.08 s → 1.84 s** (stage 7b): skip serial wait on wake (−990), page-index cache
  `/books/<book>.idx` (−~700 incl. file scan), HALF instead of RESYNC after a clean sleep (−1500; no flash, clean).
  Remaining: SD cold start 648 ms (47 ms when the card stayed powered), HALF 870, display init 223.
  Data: docs/data/wake-timeline-x3.csv.
- **Stage 11:** at sleep the clean page is saved raw into the unused `spiffs` partition (239 ms incl. erase); at a
  clean wake it's loaded and the HALF refresh starts at 376 ms, the SD cold start (648 ms, the card's own init —
  a failed 25 MHz try was ruled out) runs during the waveform. Settle (200 ms) counted from the BUSY rising edge
  (ISR timestamp). Battery wake: **page visible ≈ 0.83 s, ready 1.10 s** (was 1.84 s).
  `esp_rtc_get_time_us()` at setup reads ≈ millis() even on power-on → doesn't measure ROM/bootloader time.
- BQ27220 battery gauge (0x55): awake draw **−21 mA on battery**, −22 mA on USB once charge terminated at 100 %
  (battery feeds the board even on USB — like skill G50). After any reset/power cycle on USB the charger
  restarts a top-off: +83 → +65 mA. Current sign is the only USB/charge hint on the X3 (no VBUS pin).
- ~21 mA awake on a 650 mAh cell ≈ 30 h awake; auto power-off after 5 min idle.
- **Power bench (stage 8, BQ27220 1 s readings, 16 samples/mode, repeated twice):** 160 MHz 20.8 mA, 80 MHz 17.6,
  40 MHz 11.5, light sleep 50 ms **2.0**, 100 ms 1.8, LS 50 ms + panel rails on 2.0. Floor ≈ 1.6 mA (SD card stays
  powered on the GPIO13 latch). Reader now light-sleeps in 50 ms steps when idle and no USB host
  (HWCDC::isPlugged()); ADC-ladder keys can't wake by GPIO level, so they're polled on each timer wake.
  Data: docs/data/power-bench-x3.csv.

## Power — earlier observations
- Unplugging USB (17 s) does **not** reset or power down the chip with our firmware: uptime continuous, boot # unchanged.
- One unexplained reboot at 10:35:49 (USB gone 1.3 s, screen refreshed twice). Firmware now logs boot # + reset reason.

## SD card (source) — shares display SPI bus
| Signal | GPIO |
|---|---|
| SCLK / MOSI | 8 / 10 (shared) |
| MISO | 7 |
| CS | 12 |
| Power enable (active HIGH; drive LOW in sleep) | 13 |

## I²C bus (source) — SDA **20**, SCL **0**, 400 kHz
| Chip | Address | Role |
|---|---|---|
| TI BQ27220 | 0x55 | battery fuel gauge (650 mAh cell; SoC reg 0x2C, mV 0x08, mA 0x0C) |
| DS3231 | 0x68 | real-time clock |
| QST QMI8658 | 0x6B (alt 0x6A) | 6-axis IMU (WHO_AM_I reg 0x00 = 0x05) — tilt page-turn |
**measured:** scan of 0x08–0x77 finds exactly 0x55, 0x68, 0x6B. QMI8658 WHO_AM_I=0x05. BQ27220: 4347 mV, 100 %, −20 mA on USB. DS3231 running (not set to local time).
X4 has none of these; that's how firmware tells X3 from X4 (≥2 of 3 answer = X3).
GPIO20 is U0RXD and GPIO0 is the X4's battery ADC — on the X3 both are I²C.

## Buttons (source) — two ADC resistor ladders, 11 dB attenuation
| GPIO | Buttons (raw ADC avg, X4 data) |
|---|---|
| 1 | Back ~3512, Confirm ~2694, Left ~1493, Right ~5 |
| 2 | Up ~2242, Down ~5 |
| 3 | Power (digital, active-LOW) |
**measured on X3:** idle 4095 (both); Back 3483, Confirm 2663, Left 1469, Right 0, Up 2207, Down 4; Power LOW when pressed.
**Stage 3 (measured):** 20 ms stability debounce + 5 ms sampling → 0 glitches over all presses/holds/double-taps;
buttons sampled during e-paper refresh via driver idle hook (presses during refresh counted). Power held 5.2 s: no power-off, no reset.
Within ~30 counts of X4 → CrossPoint thresholds (3100/2090/750, 1120) apply; add a no-button threshold (~3800). Data: docs/data/buttons-x3.csv

## SD card — measured (stage 6)
- Shares the display's global `SPI` (display `SPI.begin` first, then `SD.begin(12, SPI, hz)`; `SPIClass::begin` is a no-op
  once started). Mounts first try at 25 MHz (library cap) in 47 ms. 15 GB SDHC, FAT32.
- Read 256 KB in 171 ms ≈ 1.5 MB/s (predicted 1–2 MB/s).
- Stock card layout: `XTCache/` (reading/bookmark/readTime/xtc/fmsort), `Pushed Books/*.xtc` (Xteink pre-rendered pages).
- Our reader: `/books/*.txt` (or root). Alice (174 KB) → 156 pages, exactly as the host Python simulation predicted;
  indexing 596 ms, page layout 46 ms, free heap 197 KB after indexing.

## Typography (stage 9)
- Panel ≈ 257 ppi (792×528; assumes 3.7" diagonal, unconfirmed). Adafruit GFX addresses 256 glyphs (`write(uint8_t)`),
  so fonts are generated in **cp1252 order** (Latin-1 + curly quotes/dashes/ellipsis in 0x80–0x9F) by
  `tools/fontconvert.py` (FreeType, hinted 1-bit), and the tokenizer maps UTF-8 → cp1252.
- Noto Serif (OFL 1.1, fonts/OFL.txt). Sizes 28/32/36/40 px ≈ 8/9/10/11 pt; **28 px chosen** by eye.
  First try (21 px ≈ 6 pt, matched FreeSerif's pixel size) was far too small.
- Alice pages: 21 px 159, 28 px 268, 32 px 353, 36 px 436, 40 px 533 — every device count matched the host simulation.
  Per-size index caches `/books/alice.txt.<px>.idx` (first build ~0.9 s, cached 5 ms). Reading position stored as a
  byte offset (NVS "pos") so a size change keeps your place.

## Navigation (stage 10)
- Chapter headings: paragraph whose first word is CHAPTER/LETTER (any case), ≤3 lines, ≤200 bytes (skips a table of
  contents); each chapter starts on a new page, heading centred. Chapter list stored in the v3 index cache.
- Predicted (tools/pagesim.py, a byte-level port of text_book.cpp) at 28 px: Alice 276 p / 12 ch (device: 276 / 12 ✓),
  Frankenstein 755 / 28, Pride and Prejudice 1345 / 61 (device counts not logged yet).
- UI: tap Back = menu (Contents / Books / Text size / Clean screen); Back/Confirm act on release everywhere; key events
  queued and handled outside refreshes. Per-book positions in NVS key "p"+CRC32(path). UI font Noto Sans 20 px (cp1252).

## Unknowns / to verify
- Earlier USB drops = magnetic cable detaching. One reboot (10:35:49) still unexplained.
- Is −20 mA on USB = charge terminated, battery feeding system?
- Charge detection (no charger IC; maybe via gauge current sign)

## Sources
- https://github.com/daveallie/crosspoint-reader (lib/hal/HalGPIO.h)
- https://github.com/Free-Ink/freeink-sdk (libs/hardware/BoardConfig, InputManager, XteinkDetect)
- https://learn.adafruit.com/circuitpython-on-the-xteink-x4-ereader/pinouts (X4 sibling)
