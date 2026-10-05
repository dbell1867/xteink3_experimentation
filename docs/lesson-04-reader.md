# Lesson 04 — The reader: SD card, text layout, typography and navigation

**Stages:** 6, 9, 10 · **Code:** `stages/06-text-reader/`, `09-typography/`, `10-navigation/`
**Engine:** `src/text_book.{h,cpp}` · **Tools:** `tools/pagesim.py`, `tools/fontconvert.py`

## Objectives
- Share one SPI bus between the display and the SD card (skill G25).
- Lay out a whole book on a chip with ~190 KB free RAM, without loading it into memory.
- Predict layout results on the PC before flashing — and have the device match exactly.
- Real typography (curly quotes, dashes, accents) through an 8-bit graphics library.
- Navigation: chapters, a book picker, a menu.

## SD on the display's bus
Our driver starts Arduino's **global `SPI`**; the SD library joins it with its own CS (GPIO12)
— `SD.begin(12, SPI, 25000000)` *after* `epd.begin()`. `SD.begin()` calls `spi.begin()`, which
returns early if the bus already runs (checked in the source). Mounted at 25 MHz first try,
**1.5 MB/s** (predicted 1–2 ✓). The card had Xteink's own folders (`XTCache`, `Pushed Books/*.xtc`).

## The text engine
- **Stream, don't load:** a 512-byte buffered reader with `seek`/`peek`; the book can be
  any size.
- **Tokenizer:** words and paragraph breaks (a blank line). Project Gutenberg files are
  hard-wrapped at ~70 chars, so single newlines are just spaces. The licence header/footer
  (`*** START` / `*** END`) is skipped; `Title:` is read from the header.
- **Page index:** one layout pass without drawing records the byte offset where each page
  starts (4 bytes/page). Going back a page = jump to its offset.
- **Index cache** `<book>.<px>.idx` on the SD: validated by book size + a hash of every layout
  input (margins, font metrics, all glyph widths), plus a format version. Load: ~5 ms vs ~1 s.
- **Reading position = byte offset** (NVS key per book: `"p"` + CRC32 of the path). It
  survives font changes, where page numbers don't.

## Predict before flashing
`tools/pagesim.py` is a line-by-line Python port of the layout over raw bytes. Every
device page count so far matched it **exactly**:

| Book @ font | Predicted | Device |
|---|---|---|
| Alice, FreeSerif 12pt | 156 | 156 |
| Alice, Noto Serif 21 px | 159 | 159 |
| Alice, 28 / 36 / 40 px | 268 / 436 / 533 | 268 / 436 / 533 |
| Alice + chapter breaks, 28 px | 276 p, 12 ch | 276 p, 12 ch |

## Typography: cp1252 fonts
Adafruit GFX's `write()` takes **one byte**, so a font has at most 256 glyphs — U+201C (")
can't be addressed. **Windows-1252** = Latin-1 plus curly quotes, dashes and ellipsis in the
otherwise unused 0x80–0x9F. So `tools/fontconvert.py` (FreeType, hinted 1-bit, Adafruit's
exact `GFXfont` layout) renders each cp1252 byte's Unicode glyph *at that byte*, and the
tokenizer maps UTF-8 → cp1252. It also renders a PNG preview on the PC.
Font: **Noto Serif** (OFL 1.1 — the font's own metadata says so; Arch's licence file was a
stale Apache text, replaced with the real OFL). UI: Noto Sans 20 px.

**Size mistake:** first try was 21 px — matching the old font's pixels — but the panel is
~257 ppi, so that was ~6 pt. Now 28/32/36/40 px (≈ 8–11 pt), selectable; **28 px** chosen by eye.

## Navigation
- **Chapters:** a paragraph whose first word is CHAPTER/LETTER and that has ≤ 3 lines
  (skips a table of contents, which is one long paragraph). Each chapter starts a new page,
  centred. Checked on Alice (12), Frankenstein (Letters 1–4 + 24), Pride and Prejudice (61).
- **Menu (tap Back):** Contents / Books (with % read) / Text size / Clean screen.
- **Screen state machine** (Reading / Menu / Contents / Books) + key queue (lesson 03).

## Real bugs and fixes
| Symptom | Cause | Fix |
|---|---|---|
| Reader opened `/cmdline.txt` (1 page) | a **Raspberry Pi boot card** was in the X3 | directory logging in `findBook()` |
| `/books` not found on the right card | folder misplaced / copy not flushed | `/books` at card root; eject before removing |
| Status line truncated | 96-byte `snprintf` buffer | bigger buffer |
| 21 px text tiny | ignored pixel density | selectable sizes; 28 px |
| Header can't show "Alice's" | FreeSans is ASCII-only | cp1252 Noto Sans UI font |

## C++ for a Python programmer
- `std::vector<uint32_t> pageStarts;` ≈ `list[int]`; `push_back` ≈ `append`.
- `std::upper_bound(begin, end, x)` ≈ `bisect.bisect_right(list, x)` — used by `pageForOffset`.
- `reinterpret_cast<const uint8_t*>(&hdr)` — "treat this struct's memory as bytes" to write a
  binary header; like `struct.pack`, but with no copy.
- `[&](...) { ... }` is a lambda that can modify outer variables (Python closures with `nonlocal`).

## Cheat-sheet
```bash
python tools/pagesim.py include/NotoSerif28.h books/*.txt                     # predict pages/chapters
python tools/fontconvert.py FONT.ttf 28 NotoSerif28 > include/NotoSerif28.h  # make a font
python tools/fontconvert.py FONT.ttf 28 X --preview out.png "“Quotes” — test…" # look first
```

## Glossary
- **cp1252** — an 8-bit encoding: ASCII + Latin-1 + typographic punctuation in 0x80–0x9F.
- **Hinting** — snapping glyph outlines to the pixel grid so 1-bit text stays crisp.
- **NVS** — the ESP32's key/value storage in flash (`Preferences`).
