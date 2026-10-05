# Lesson 02 — E-paper: first light, orientation, refresh modes and ghosting

**Stages:** 2, 2b, 4, 5 · **Code:** `stages/02-first-light/`, `02b-portrait-bezel/`, `04-page-turns/`, `05-ghost-management/`
**Driver:** `src/epd_uc8253.{h,cpp}`, waveforms `include/uc8253_x3_luts.h`

## Objectives
- Write a small driver for the UC8253 from a known-good reference, and understand every step.
- Learn what makes e-paper different: slow, bistable, waveform-driven, and prone to ghosting.
- Choose refresh modes by **measuring** time and **rating** quality, not by guessing.

## How this panel works (the mental model)
- Two RAM planes: **DTM1 = "old" image, DTM2 = "new" image**. A refresh moves each pixel from
  old to new using a **waveform** (LUT): a table of voltage steps for each transition
  (white→white, black→white, …).
- **BUSY** is "two-phase" on the X3: after a command it drops LOW (working), then rises HIGH (done).
- The image stays on the glass with **zero power**. Power is only spent changing it.
- Rows are sent **bottom first** ("flipped") because of how the panel is mounted; 1 bit = white.

## Decisions
| Decision | Why |
|---|---|
| Own minimal driver, following freeink-sdk's `Uc8253X3Driver` | learn by reading; copy sequence, not the whole SDK |
| Waveform tables copied from the SDK (MIT) | **verified byte-for-byte in our factory dump** (FULL at 0x79485, NORMAL at 0x793A9) |
| Adafruit GFX `GFXcanvas1` as framebuffer | its 1-bpp row layout is exactly what the controller wants; 52 KB fits without PSRAM |
| FAST for page turns + RESYNC every 15 | measured below |

## Stage 2 — First light (one FULL refresh, timed — skill G44/G46)
Test pattern where each element answers a question you check by eye: border (all edges
visible?), corner labels (orientation?), 1/2/4 px stripes (bit order, SPI integrity), black block.
Measured: init 169 ms, upload 90 ms, power-on 125, **refresh 925**, power-off 52 → **1.19 s**
(predicted 1–3 s ✓).

## Stage 2b — Orientation and the bezel
- The landscape frame appeared rotated: portrait with the Power key on top = GFX
  **`setRotation(3)`** (predicted from GFX's `drawPixel` source ✓).
- **Edge rulers** (marks at 0, 2, … 14 px from each edge): "4" fully visible, "2" a sliver →
  the bezel hides **3 px on every side**. The SDK's default guess (9/3/3/3) was off.

## Stage 4 — Faster page turns
| Mode | Waveform | Total | Looked | In factory fw? |
|---|---|---|---|---|
| FULL | 925 ms | 1.7 s | clean, flashes | yes |
| **FAST** (community "turbo") | 378 ms | **467 ms** | **cleanest of the fast ones** | no |
| NORMAL (OEM tables, SDK conditioning setup) | 455 ms | 744 ms | **messier** | yes |
| HALF ("scrub") | 454 ms | 743 ms (870 incl. power-on) | — | built from OEM tables |

Prediction "NORMAL looks cleaner than FAST" **failed**: the SDK only uses those OEM tables as a
conditioning pass, and we copied that setup — not how Xteink's firmware drives them for turns.
Keeping the panel's high-voltage rails on between turns saves the 127 ms power-on (and costs
~nothing — lesson 05).

## Stage 5 — Ghosting
Stress test: 20 automatic FAST turns, then rate ghosting 0–3 by eye.

| After | Rating | Cost |
|---|---|---|
| 20 FAST turns | 2 | 467 ms each |
| HALF | 1 | 743 ms |
| **RESYNC** (FULL + one OEM conditioning pass + no-op FAST settle — CrossPoint's X3 clean-up) | **0** | 2259 ms |

All three predictions held. Policy: **FAST turns, RESYNC every 15** (CrossPoint's default).

## Real bugs and fixes
| Symptom | Cause | Fix |
|---|---|---|
| A redundant POWER_ON would hang ~1 s | a powered UC8253 gives no BUSY edge | track `isPowered`, only power on when off |
| First FAST after FULL garbles (SDK note) | old-frame RAM not settled | no-op FAST pass after FULL |
| RESYNC requested but first boot skipped conditioning | conditioning decided before mode upgrades | decide after the upgrade rules |
| Two NORMAL turns had 175/368 ms waveforms on rapid re-press | unexplained | logged as open question |

## C++ for a Python programmer
- `enum class Mode : uint8_t { Full, Half, Normal, Fast };` ≈ `class Mode(IntEnum)`; you write
  `Mode::Fast`, and it's stored in one byte.
- `struct Stats { uint32_t uploadMs = 0; … };` ≈ a `@dataclass` with defaults; `stats = Stats{};` resets it.
- `void (*idleHook)() = nullptr;` is a **function pointer** — a callback slot, like storing a
  function in a variable. The driver calls it while waiting on BUSY so buttons keep working.
- `using namespace uc8253;` inside a function ≈ `from uc8253 import *` but only in that function.

## Cheat-sheet
```cpp
epd.begin();                                   // reset + init + clear both RAM planes
epd.refresh(canvas.getBuffer(), Mode::Fast);   // or Full / Half / Normal / Resync
epd.powerOff();  epd.deepSleep();              // rails off; controller deep sleep (0x07, 0xA5)
canvas.setRotation(3);                         // portrait, Power key on top
```

## Glossary
- **LUT / waveform** — the voltage sequence for each pixel transition.
- **Differential refresh** — only pixels that differ between DTM1 and DTM2 are driven (FAST, NORMAL).
- **Ghosting** — faint leftovers of earlier images; differential refreshes accumulate it.
- **CDI** — VCOM and data-interval setting (cmd 0x50); each waveform bank has its own value.
