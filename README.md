# Xteink X3 — custom firmware for reading lessons and presenting

Custom firmware for the **Xteink X3** e-paper reader (ESP32-C3), plus PC tools, built
step by step as a learning project. It turns the X3 into two things:

1. **A reader for my own lesson notes.** Markdown lessons are rendered on the PC into
   pre-made pages (tables and code included) and read on the X3, e.g. on the bus.
2. **A presenter for [marimo](https://marimo.io) slide decks.** The X3 shows the speaker
   notes for the current slide and works as a Bluetooth clicker: its keys change the
   slides on the laptop.

Getting files onto it is automatic: edit a lesson or a deck's notes, plug the X3 into the
PC, and the changed files are rendered and sent over USB.

> **Not affiliated with Xteink.** Flashing this replaces the stock firmware. Back up your
> device's flash first (see [Back up first](#back-up-first)). Pins and drivers are for the
> X3 only — other models differ.

## What it does

| On the X3 | |
|---|---|
| Plain-text books | Laid out on the device in Noto Serif (28–40 px), chapters, contents, saved positions |
| Pre-rendered pages (`.xtc`) | Lessons and talks from the PC, landscape, copied straight to the screen |
| Books menu | One folder per lesson series, plus `talks/`; shows when the PC last synced |
| Bluetooth clicker | Page Down / Page Up to the laptop, resync to the shown step, switches itself off after 30 min |
| Battery life | Light sleep between key presses (≈2 mA), powers off when idle, back on the page in ≈1 s |

| On the PC | |
|---|---|
| `x3` command | `add`, `status`, `build`, `sync`, `rm`, `auto` — keeps the X3 in step with a library folder |
| Auto-sync | Plugging in the X3 syncs it (udev + a systemd user service) and shows a desktop notification |
| Renderers | Markdown → pages (`tools/mdpages.py`); marimo speaker notes → one page per slide step (`tools/notes2xtc.py`) |
| Checker | `tools/xtcinfo.py` reads `.xtc` files independently of the writer |

## Repository layout

```
src/, include/     firmware (PlatformIO, Arduino core 3.x / ESP-IDF 5.5)
tools/             PC side: x3.py (the x3 command), renderers, font converter, autosync/
docs/              hardware reference, lessons 01-06 (how each stage was built), PLAN.md
docs/data/         measurements: button readings, power bench logs
stages/            a snapshot of the firmware after each stage (01 ... 16)
books/             three public-domain test books (Project Gutenberg)
fonts/             font licence (OFL) and how the fonts were generated
backup/            restore instructions for the factory flash (the dump itself is not in git)
```

## Hardware

ESP32-C3 (RISC-V, 160 MHz, **no PSRAM**), 16 MB flash (**must be flashed in DIO mode**),
792×528 mono e-paper (UC8253), microSD, BQ27220 battery gauge, DS3231 clock, QMI8658 IMU,
7 keys on two ADC ladders plus Power. Full pin map, sources and measurements:
[docs/hardware-x3.md](docs/hardware-x3.md).

## Getting started

**Needs:** Linux, [PlatformIO](https://platformio.org), [uv](https://docs.astral.sh/uv/),
the Noto fonts (`noto-fonts` on Arch; the renderers read `/usr/share/fonts/noto/`).

### Back up first
```bash
esptool --chip esp32c3 --port /dev/ttyACM0 read-flash 0 0x1000000 backup/factory.bin
```
Wake the X3 first (it drops off USB when asleep). If the read stalls, see
[backup/README.md](backup/README.md), which also has the restore command.

### Flash the firmware
```bash
pio run -t upload
```
Put books on the SD card in `/books` (`.txt` or `.xtc`), or sync them over USB (below).

### Install the PC tools
```bash
uv tool install --editable .        # puts `x3` on your PATH
x3 add ~/path/to/project/docs --include 'lesson-*.md'   # a lesson series
x3 add ~/path/to/marimo-deck                           # a deck with a slides layout
x3 sync                                                # render + send what changed
```
The library is `~/X3`: symlinks to your folders, so nothing is copied. `x3 sync` never
deletes; `x3 rm` does, after asking.

### Optional: sync on plug-in
```bash
cp tools/autosync/x3-sync.service ~/.config/systemd/user/ && systemctl --user daemon-reload
sudo cp tools/autosync/70-x3-sync.rules /etc/udev/rules.d/ && sudo udevadm control --reload
```
**Edit the rule first:** it matches one X3 by its USB serial number (the chip's MAC),
so other ESP32 boards don't trigger it. Find yours with
`udevadm info -q property -n /dev/ttyACM0 | grep ID_SERIAL_SHORT`.
Before flashing firmware, pause it with `x3 auto off` (each upload re-plugs the X3).

## Using it

- **Keys:** lower page key = next. Tap Back = menu (Contents, Books, text size, clean
  screen, orientation, clicker). Hold Power = sleep / power off.
- **Presenting:** menu → Clicker → pair "X3 Clicker" once on the laptop → open the talk
  in `talks/`. Each key press changes the slide, then the notes page. Tap Confirm to
  resync the laptop to the step on the X3.

## How it was built

Each stage starts with a prediction and ends with a measurement on the device — including
the ones that turned out wrong. The lessons explain the why:

| Lesson | Stages |
|---|---|
| [01 — identify, back up, probe](docs/lesson-01-identify-backup-probe.md) | which chip, factory backup, pin probe |
| [02 — e-paper](docs/lesson-02-epaper.md) | own display driver, refresh modes, ghosting |
| [03 — buttons](docs/lesson-03-buttons.md) | ADC key ladders, debounce, long press |
| [04 — reader](docs/lesson-04-reader.md) | SD card, text layout, fonts, navigation |
| [05 — power](docs/lesson-05-power.md) | battery latch, light sleep, fast wake |
| [06 — lessons on the bus, talking points, clicker](docs/lesson-06-pc-pipeline-presenter.md) | XTC pages, USB sync, talking points, BLE clicker, auto-sync |

Current status and open questions: [docs/PLAN.md](docs/PLAN.md).

## Credits

- [CrossPoint Reader](https://github.com/daveallie/crosspoint-reader) and
  [freeink-sdk](https://github.com/Free-Ink/freeink-sdk) — pin map, display init and the
  XTC format were learned from their code.
- [Noto fonts](https://github.com/notofonts) — SIL Open Font License 1.1 ([fonts/](fonts/)).
- [Project Gutenberg](https://www.gutenberg.org) — the test books.
- [marimo](https://marimo.io) — slides and speaker notes.

## Licence

[MIT](LICENSE) for the code and docs in this repo. The fonts (SIL OFL 1.1, [fonts/](fonts/))
and the Project Gutenberg books keep their own licences, and the generated font headers in
`include/Noto*.h` fall under the OFL.
