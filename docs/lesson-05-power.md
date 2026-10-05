# Lesson 05 — Power: battery, power-off, light sleep and fast wake

**Stages:** 7, 7b, 8, 11 · **Code:** `stages/07-battery-sleep/`, `07b-fast-wake/`, `08-light-sleep/`, `11-faster-wake/`
**Modules:** `src/battery.*`, `src/power_bench.*`, `src/wake_frame.*`

## Objectives
- Read the battery with the on-board fuel gauge, and use it as a **power meter**.
- Understand how the X3 switches itself off, and do it properly.
- Cut idle current, and cut wake time — measuring before each change (skill G20).

## How the X3 is powered (found by experiment)
- **GPIO13 is the battery latch** (and the SD card's power). Pressing Power connects the
  battery; firmware must drive GPIO13 HIGH — as its **very first action** — to stay on.
  Driving it LOW **on battery cuts all power**. Proof: after a battery "sleep", the next boot
  reported reset reason 1 (power-on) and the RTC boot counter was wiped.
- **On USB**, the same code is a real **deep sleep**; the Power key (GPIO3) wakes it
  (reset reason 8, wake cause 7 = GPIO).
- There's **no USB-detect pin**; the gauge's current sign is the only hint (+ = charging).
  With a full battery the **battery powers the board even on USB** (−22 mA on USB,
  −21 mA on battery) — the skill's G50 situation. The charger restarts a top-off after any
  power cycle (+83 mA).

## The gauge as a power meter (stage 8)
Hold Confirm (on battery) → six modes × 20 s, 16 one-second gauge samples each, no screen
updates while measuring. Repeated twice; rows agreed within 0.1 mA.

| Mode | mA | Predicted |
|---|---|---|
| 160 MHz, `delay(1)` loop | 20.8 | 21 ✓ |
| 80 MHz | 17.6 | 14 ✗ (peripheral clock stays 80 MHz) |
| 40 MHz | 11.5 | 10 ≈ |
| **light sleep, 50 ms timer wake** | **2.0** | 2 ✓ |
| light sleep, 100 ms | 1.8 | 1.5 ✓ (floor ~1.6 mA: SD stays powered on the latch) |
| light sleep 50 ms + panel rails on | 2.0 | +1–3 ✗ (rails cost < 0.5 mA) |

The reader light-sleeps in 50 ms steps when idle **and no computer is on the USB bus**
(`HWCDC::isPlugged()`, skill G17e). Ladder keys can't wake by GPIO level, so each timer
wake polls them; once a key is active it stays awake. Felt latency: none; missed presses: 0.
Idle draw 20.8 → 2.0 mA ≈ 31 h → ~13 days of idle-awake time on 650 mAh.

## Wake time — measured, then cut
Firmware records a timeline (ms at each boot phase) in NVS; the next USB boot prints it.

| Step | What changed | Page visible | Ready |
|---|---|---|---|
| baseline | — | ~5.0 s | 5.08 s |
| 7b | no serial wait on a key wake (−990), page-index cache (−~700), HALF instead of RESYNC after a clean sleep (−1500) | ~1.8 s | 1.84 s |
| 11a | page saved raw in flash (`spiffs` partition) at sleep; HALF starts from it **before** the SD card mounts; SD's 648 ms cold start runs during the 454 ms waveform | **0.83 s** | 1.30 s |
| 11b | settle (200 ms) timed from a **BUSY-rise interrupt**, so it overlaps too | 0.83 s | **1.10 s** (predicted 1.10) |

Hypothesis "the slow cold SD mount is a failed 25 MHz attempt" was **falsified** — the 25 MHz
try succeeds; the 648 ms is the card's own initialisation. `esp_rtc_get_time_us()` at `setup()`
read ≈ `millis()` even after a power-on, so ROM/bootloader time stays unmeasured (G29: an
instrument that can't disagree isn't evidence).

## Real bugs and fixes
| Symptom | Cause | Fix |
|---|---|---|
| Light-sleep timer wake would also end a later deep sleep after 50 ms | wake sources persist | disarm all sources after each light sleep |
| Battery latch could float in light sleep | pads not held | `gpio_hold_en(GPIO13)` around light sleep |
| Re-wake right after sleeping | Power key still down (G19d) | wait for release before arming the wake |
| Board vanished mid-development | 5 min auto-sleep on USB | flash with a wait-for-port loop + press Power |
| Two tests "didn't work" | ran before the flash finished / sleep path not reached | confirm flash first; check kernel USB log + NVS markers |

## C++ for a Python programmer
- `RTC_NOINIT_ATTR uint32_t bootCount;` — a global that survives a reset but not a power
  cut. Comparing it across boots is how we proved the latch cuts power.
- `void IRAM_ATTR onBusyRise()` — an interrupt handler: kept in fast RAM, does almost
  nothing (stores `millis()` in a `volatile` variable) — no Serial, no SPI.
- `template <typename F> void hold(..., F sample)` — accepts any callable, like duck typing.

## Cheat-sheet
```cpp
battery::read();                       // {ok, percent, mV, mA}  (+mA = charging)
power_bench::lightSleep(50);           // timer + Power-key wake, latch held
wake_frame::save(fb, len, meta);       // at sleep;  wake_frame::load(...) at wake
epd.refreshStart(fb, Mode::Half);  /* mount SD, open book */  epd.refreshFinish();
```

## Glossary
- **Latch** — a switch the firmware must hold on to keep its own power.
- **Light sleep** — CPU paused, RAM kept, resumes in place; **deep sleep** — reboot on wake.
- **Fuel gauge** — a chip that measures battery voltage, current and state of charge.
