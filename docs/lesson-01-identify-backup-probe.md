# Lesson 01 — Identify an unknown device, back it up, probe its hardware

**Device:** Xteink X3 e-reader · **Stages:** 0–1 · **Date:** 2026-10-05
**Code:** `stages/01-hardware-probe/` · **Reference:** `docs/hardware-x3.md`

## Objectives
- Find out what microcontroller is inside a closed consumer device, without opening it.
- Make a verified, restorable copy of the factory firmware *before* writing anything.
- Turn community research into a pin map, then prove each claim on the real unit.
- Get our own C++ running and talking over USB serial.

## Hardware (as found)
| Part | What | How we know |
|---|---|---|
| MCU | ESP32-C3, single-core RISC-V 160 MHz, Wi-Fi + BLE 5 | `esptool chip-id` |
| Flash | 16 MB Puya (JEDEC 0x85/0x2018), **DIO only** | `esptool flash-id`; G55 |
| RAM | ~400 KB internal SRAM, **no PSRAM** | the C3 has none |
| USB | native USB-Serial/JTAG (303a:1001) → `/dev/ttyACM0` | kernel log |
| Display | 792×528 e-paper, **UC8253** controller (SPI) | probe: FLG=0x13, no VER |
| I²C (SDA 20, SCL 0) | BQ27220 gauge 0x55, DS3231 RTC 0x68, QMI8658 IMU 0x6B | full-range scan |
| Keys | two ADC resistor ladders (GPIO1, GPIO2) + Power (GPIO3) | raw ADC readings |

## Stage 0 — "Is it even an ESP32?"
1. `lsusb` showed **nothing**: the stock firmware puts the chip to sleep and the USB
   device disappears with it.
2. The **kernel log remembers**: `journalctl -k` had `idVendor=303a … Manufacturer: Espressif,
   Product: USB JTAG/serial debug unit`. 303a is Espressif's vendor ID, and that product
   string is the built-in USB of the C3/S3/C6 family. The serial number *is* the MAC address.
3. Woke the reader, ran `esptool chip-id` → **ESP32-C3 rev v0.4**, 16 MB flash.

> **Lesson:** when a device vanishes, the kernel log is a free history of every time it was there.

## Stage 0b — Back up the factory flash (skill G42)
`read-flash 0 ALL` failed with `Packet content transfer stopped` — at **the same address
every time** (0x400000). That's deterministic, so retrying is pointless; change the *method*:
- read in 1 MB chunks → only the 0x400000 chunk fails;
- bisect 64 KB → 16 KB: two blocks (0x424000, 0x454000) always stall the fast "stub" reader;
- a 4 KB read *at* 0x400000 works → it's the data pattern + transfer, not the address;
- the ROM's own slow reader (`--no-stub`) reads them fine → chunks with a ROM fallback;
- `verify-flash` (the **chip** hashes its flash and compares) → "digest matched".

Result: `backup/factory.bin` (read-only, SHA-256 alongside, restore command in `backup/README.md`).
It's **not in git** — it holds the stock firmware and its Wi-Fi settings (see git history notes in PLAN.md).

## Stage 0c — Research, then verify
CrossPoint Reader (open-source X3/X4 firmware) and its `freeink-sdk` had an X3 board profile,
reverse-engineered from the stock firmware. We copied *facts*, not code, into
`include/board_pins.h`, and treated every one as a hypothesis (skill G27). The factory dump
helped too: `strings` showed the button code reads ADC values, and the project name
`arduino-lib-builder` showed the stock firmware is Arduino-ESP32 — the same stack we use.

## Stage 1 — Hello + hardware probe (read-only)
One sketch that only *reads*: chip info, a full I²C scan (0x08–0x77, not just where we
expected), the display-controller probe, and a live button monitor.

**Display controller probe — designed so it can disagree.** UC81xx controllers answer
command 0x71 (status, FLG) and 0x70 (version, VER) over the half-duplex MOSI line; a UC8253
has no VER. Silence alone could mean "the read method is broken". We got **FLG = 0x13** (a
real driven status, idle bit set — so the read path works) and **VER = FF FF FF FF FF** (no
version register) → UC8253, confidently.

## Real bugs and fixes
| Symptom | Cause | Fix |
|---|---|---|
| `lsusb` empty | stock fw sleeps; USB goes with it | kernel log; wake before `esptool` |
| backup read stalls at 0x400000 every retry | stub transfer stalls on certain data | chunks + `--no-stub` fallback + `verify-flash` (G42) |
| first flash: all hashes verified, then endless `partition 0 invalid magic number 0xffff` | generic C3 board JSON = QIO; QIO bootloader on DIO-only flash reads 0xFF | `board_build.flash_mode = dio` (**G55**, added to the skill) |
| I²C error lines from 0x6A | probing the IMU's *alternate* address where nothing lives | only try the alternate if the main address is silent |
| USB "dropped" mid-test | magnetic cable detached | not a bug — confirmed by kernel log + uptime counter (lesson 05) |

## C++ for a Python programmer
- `#include "board_pins.h"` ≈ `import board_pins`; `namespace pins { constexpr int EPD_CS = 21; }`
  ≈ a module of constants — `constexpr` means "known at compile time", it costs no RAM.
- `bool i2cReadRegs(uint8_t addr, uint8_t reg, uint8_t* out, uint8_t len)` — `uint8_t*` is a
  **pointer**: the caller passes the address of a buffer and the function fills it, like
  passing a `bytearray` to be mutated. Returning `bool` instead of raising is the C++ habit
  on microcontrollers (exceptions are costly).
- `for (uint8_t addr : {i2caddr::QMI8658, uint8_t(0x6A)})` ≈ `for addr in (0x6B, 0x6A):`.

## Cheat-sheet
```bash
journalctl -k | grep -iE 'usb|303a'                        # when did it appear/vanish?
esptool --port /dev/ttyACM0 chip-id                         # what chip?
esptool --chip esp32c3 --port /dev/ttyACM0 --after no-reset read-flash 0x400000 0x100000 part.bin
esptool --chip esp32c3 --port /dev/ttyACM0 --no-stub ...    # slow ROM reader (G42)
esptool --chip esp32c3 --port /dev/ttyACM0 verify-flash 0 backup/factory.bin
esptool --chip esp32c3 --port /dev/ttyACM0 write-flash 0 backup/factory.bin   # restore stock
pio run -t upload                                           # build + flash ours
```

## Glossary
- **USB-Serial/JTAG** — the C3's built-in USB block; looks like a serial port, also resets/flashes the chip.
- **Stub flasher** — esptool's small fast loader it uploads into RAM; `--no-stub` uses the ROM's slower built-in one.
- **QIO / DIO** — flash read modes using 4 or 2 data lines. The bootloader must match what the chip supports.
- **Half-duplex SPI** — one data line used both ways (we send a command, then *listen* on MOSI).
- **Deterministic failure** — fails the same way every time; repeat attempts add no information (G42).
