// Stage 10 — text reader with navigation (contents, book picker, menu).
//
// Reading:
//   Right / Down -> next page        Left / Up -> previous page
//   hold Right / Left -> jump 10 pages
//   tap Back -> menu (Contents / Books / Text size / Clean screen)
//   hold Power -> sleep now (also after 5 min idle)
//   hold Confirm -> power bench (battery only, ~2.5 min)
// Menus: Up/Down/Left/Right move, tap Confirm selects, tap Back goes back.
//   Back and Confirm act on RELEASE everywhere, so a key that closes a menu
//   can't have its release reopen it in the next screen.
//
// FAST page turns + RESYNC every 15; light sleep in 50 ms steps when idle on
// battery (20.8 -> 2.0 mA). Each book's reading position is a byte offset in
// NVS, so it survives font changes, sleep and power-off.
//
// "Sleep" on the X3: GPIO13 gates the battery (CrossPoint: "driving it low is
// the battery power-off"). On battery the chip loses power entirely and the
// Power key cold-boots it; on USB the chip stays powered, so it's a real deep
// sleep woken by the Power key (GPIO3). Either way we resume from NVS.

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSerif12pt7b.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <driver/gpio.h>
#include <esp_rom_crc.h>
#include <esp_rtc_time.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include <algorithm>
#include <vector>

#include "NotoSans20.h"
#include "NotoSerif28.h"
#include "NotoSerif32.h"
#include "NotoSerif36.h"
#include "NotoSerif40.h"
#include "battery.h"
#include "board_pins.h"
#include "buttons.h"
#include "epd_uc8253.h"
#include "power_bench.h"
#include "text_book.h"
#include "wake_frame.h"

using Mode = EpdUc8253::Mode;

GFXcanvas1 canvas(EpdUc8253::WIDTH, EpdUc8253::HEIGHT);
EpdUc8253 epd;
Buttons buttons;
TextBook book;
Preferences prefs;  // key/value store in the ESP32's flash (NVS)

constexpr uint16_t WHITE = 1;
constexpr uint16_t BLACK = 0;
constexpr uint32_t RAILS_OFF_AFTER_MS = 30000;
constexpr int CLEAN_EVERY = 15;
constexpr uint32_t SLEEP_AFTER_MS = 5 * 60 * 1000;  // auto power-off when idle
constexpr uint32_t LIGHT_SLEEP_MS = 50;            // bench: 2.0 mA (100 ms: 1.8 mA, not worth 2x latency)
constexpr uint32_t STAY_AWAKE_AFTER_KEY_MS = 300;
constexpr size_t FB_LEN = EpdUc8253::ROW_BYTES * EpdUc8253::HEIGHT;  // 52,272 bytes

// Portrait page geometry (528 x 792; the bezel hides 3 px per side).
constexpr int PW = 528, PH = 792;
const GFXfont* const UI_FONT = &NotoSans20;  // header, menus: cp1252 like the body fonts
// Body text sizes (Noto Serif, cp1252 layout: real quotes/dashes/accents; tools/fontconvert.py).
// The panel is ~257 ppi, so 28/32/36/40 px em ~ 8/9/10/11 pt.
struct FontChoice {
  const GFXfont* font;
  int px;
};
const FontChoice FONTS[] = {{&NotoSerif28, 28}, {&NotoSerif32, 32}, {&NotoSerif36, 36}, {&NotoSerif40, 40}};
constexpr int NUM_FONTS = sizeof(FONTS) / sizeof(FONTS[0]);
int fontIndex = 0;  // 28 px: read best on the device (stage 9); choice saved in NVS "font"

TextBook::Layout LAYOUT = {
    FONTS[0].font,
    28, PW - 28,  // left, right
    58, PH - 44,  // top, bottom
    10,           // paragraph gap
    24,           // indent
};

// Reboot detector (survives a reset, not a power cut).
RTC_NOINIT_ATTR uint32_t bootMagic;
RTC_NOINIT_ATTR uint32_t bootCount;

// Wake timeline: millis() at each boot phase, saved in NVS "wake".
struct Mark {
  const char* name;
  uint32_t ms;
};
Mark marks[12];
int numMarks = 0;
void mark(const char* name) {
  if (numMarks < 12) marks[numMarks++] = {name, millis()};
}
uint32_t bootRomMs = 0;  // RTC time at setup() = ROM + bootloader + app start (RTC timer starts at power-on)
bool timelineSaved = false;
bool showBootInfo = true;  // first page after boot shows boot # / reset reason in the header

// ------------------------------------------------------------------ state ---

enum class Screen { Reading, Menu, Contents, Books };
Screen screen = Screen::Reading;

String bookPath;
std::vector<String> bookPaths;  // all .txt books found, sorted
int page = 0;
int pagesUntilClean = CLEAN_EVERY;
int sel = 0;  // selected row in the current menu/list
uint32_t lastActivity = 0;
uint32_t lastKeyEvent = 0;
BatteryReading batt;

// Key events are queued by the Buttons callback (which can run during a
// refresh, via the idle hook) and handled in loop(), never mid-refresh.
struct KeyEvent {
  Button b;
  ButtonEvent e;
  uint32_t held;
};
KeyEvent keyQueue[16];
uint8_t qHead = 0, qTail = 0;  // ring buffer (Python: collections.deque(maxlen=16))

void onButton(Button b, ButtonEvent e, uint32_t heldMs) {
  lastKeyEvent = millis();
  const uint8_t next = (qTail + 1) % 16;
  if (next != qHead) {  // drop the event if the queue is full
    keyQueue[qTail] = {b, e, heldMs};
    qTail = next;
  }
}

// ---------------------------------------------------------------- helpers ---

// NVS key for a book's position: "p" + CRC32 of its path (NVS keys max 15 chars).
String posKey(const String& path) {
  char k[12];
  snprintf(k, sizeof(k), "p%08lx", (unsigned long)esp_rom_crc32_le(0, (const uint8_t*)path.c_str(), path.length()));
  return String(k);
}

int textWidth(const GFXfont* f, const String& s) {
  int w = 0;
  for (size_t i = 0; i < s.length(); i++) {
    const uint8_t c = s[i];
    if (c >= f->first && c <= f->last) w += f->glyph[c - f->first].xAdvance;
  }
  return w;
}

// Cut `s` so it fits `maxW` pixels in font `f`, adding "…" (cp1252 0x85).
String fitText(const GFXfont* f, String s, int maxW) {
  if (textWidth(f, s) <= maxW) return s;
  while (s.length() > 1 && textWidth(f, s + "\x85") > maxW) s.remove(s.length() - 1);
  return s + "\x85";
}

String bookName(const String& path) {  // "/books/pride-and-prejudice.txt" -> "pride-and-prejudice"
  String n = path.substring(path.lastIndexOf('/') + 1);
  if (n.endsWith(".txt") || n.endsWith(".TXT")) n.remove(n.length() - 4);
  return n;
}

void whileRefreshing() { buttons.update(); }

// Refresh with the ghost policy: FAST, but every CLEAN_EVERY-th FAST is a RESYNC.
void refreshTurn() {
  Mode m = Mode::Fast;
  if (--pagesUntilClean <= 0) {
    m = Mode::Resync;
    pagesUntilClean = CLEAN_EVERY;
  }
  epd.refresh(canvas.getBuffer(), m);
}

// --------------------------------------------------------------- reading ---

void drawChrome() {
  canvas.setFont(UI_FONT);
  // Right side: battery (+ = charging) and page number
  char right[32];
  if (batt.ok)
    snprintf(right, sizeof(right), "%s%u%%   %d/%d", batt.mA > 0 ? "+" : "", batt.percent, page + 1, book.pageCount());
  else
    snprintf(right, sizeof(right), "%d/%d", page + 1, book.pageCount());
  const int rw = textWidth(UI_FONT, right);
  canvas.setCursor(PW - 28 - rw, 32);
  canvas.print(right);

  // Left side: current chapter (or the book title before the first chapter)
  String left;
  if (showBootInfo) {  // boot evidence, visible even on battery
    char b[48];
    snprintf(b, sizeof(b), "#%lu r%d w%d  @%lums", (unsigned long)bootCount, (int)esp_reset_reason(),
             (int)esp_sleep_get_wakeup_cause(), (unsigned long)millis());
    left = b;
  } else {
    const int ch = book.chapterForPage(page);
    left = ch >= 0 ? book.chapters()[ch].title : book.title();
  }
  canvas.setCursor(28, 32);
  canvas.print(fitText(UI_FONT, left, PW - 56 - rw - 16));
  canvas.drawFastHLine(28, 42, PW - 56, BLACK);

  // Progress bar along the bottom
  const int barW = PW - 56;
  canvas.drawFastHLine(28, PH - 20, barW, BLACK);
  canvas.fillRect(28, PH - 22, barW * (page + 1) / book.pageCount(), 5, BLACK);
}

void renderPage() {
  canvas.setRotation(3);
  canvas.fillScreen(WHITE);
  canvas.setTextColor(BLACK);
  batt = battery::read();
  book.drawPage(canvas, LAYOUT, page);
  drawChrome();
}

void savePosition() { prefs.putUInt(posKey(bookPath).c_str(), book.pageStart(page)); }

wake_frame::Meta frameMeta(const String& path, uint32_t pos) {
  return {esp_rom_crc32_le(0, (const uint8_t*)path.c_str(), path.length()), pos, (uint8_t)FONTS[fontIndex].px};
}

// First screen after boot: record the boot timeline (incl. the refresh) in NVS.
void saveTimeline(const char* refreshInfo) {
  if (timelineSaved) return;
  mark("page shown");
  String t = "rtc@setup=" + String(bootRomMs) + " ";
  for (int i = 0; i < numMarks; i++) t += String(marks[i].name) + "=" + marks[i].ms + " ";
  t += String("(") + refreshInfo + ")";
  prefs.putString("wake", t);
  Serial.printf("wake timeline: %s\n", t.c_str());
  timelineSaved = true;
}

void showPage(Mode mode) {
  const uint32_t t0 = millis();
  renderPage();
  const uint32_t layoutMs = millis() - t0;
  epd.refresh(canvas.getBuffer(), mode);
  Serial.printf("[%7lu] page %d/%d  layout %lu ms + %s %lu ms   batt %u%% %d mA\n", (unsigned long)millis(), page + 1,
                book.pageCount(), (unsigned long)layoutMs, EpdUc8253::modeName(epd.stats.used),
                (unsigned long)epd.stats.totalMs, batt.percent, batt.mA);
  savePosition();
  saveTimeline((String(EpdUc8253::modeName(epd.stats.used)) + " " + epd.stats.totalMs + " ms").c_str());
  showBootInfo = false;
  lastActivity = millis();
}

// ------------------------------------------------------------------ menus ---

// A titled list with one selected (inverted) row; scrolls to keep it visible.
void drawList(const String& title, const std::vector<String>& rows, int selected, const char* hint) {
  canvas.setRotation(3);
  canvas.fillScreen(WHITE);
  canvas.setTextColor(BLACK);
  canvas.setFont(UI_FONT);
  canvas.setCursor(28, 40);
  canvas.print(fitText(UI_FONT, title, PW - 56));
  canvas.drawFastHLine(28, 52, PW - 56, BLACK);

  const GFXfont* f = &NotoSerif28;
  canvas.setFont(f);
  const int rowH = f->yAdvance + 10;
  const int top = 66, bottom = PH - 50;
  const int visible = (bottom - top) / rowH;
  const int n = rows.size();
  const int first = constrain(selected - visible / 2, 0, max(0, n - visible));
  for (int i = first; i < n && i < first + visible; i++) {
    const int y = top + (i - first) * rowH;
    const bool hi = (i == selected);
    if (hi) canvas.fillRect(20, y, PW - 40, rowH, BLACK);
    canvas.setTextColor(hi ? WHITE : BLACK);
    canvas.setCursor(32, y + rowH - 14);
    canvas.print(fitText(f, rows[i], PW - 64));
  }
  canvas.setTextColor(BLACK);
  canvas.setFont(UI_FONT);
  canvas.setCursor(28, PH - 18);
  char foot[64];
  snprintf(foot, sizeof(foot), "%s   %d/%d", hint, n ? selected + 1 : 0, n);
  canvas.print(foot);
}

std::vector<String> menuRows() {
  return {"Contents", "Books", String("Text size: ") + FONTS[fontIndex].px + " px", "Clean screen"};
}

std::vector<String> contentsRows() {
  std::vector<String> r;
  const int current = book.chapterForPage(page);
  for (int i = 0; i < (int)book.chapters().size(); i++)
    r.push_back(String(i == current ? "\x95 " : "   ") + book.chapters()[i].title);  // 0x95 = bullet
  if (r.empty()) r.push_back("(no chapters found)");
  return r;
}

std::vector<String> booksRows() {
  std::vector<String> r;
  for (const String& p : bookPaths) {
    File f = SD.open(p);
    const uint32_t size = f ? f.size() : 0;
    f.close();
    const uint32_t pos = prefs.getUInt(posKey(p).c_str(), 0);
    const int pct = size ? int(uint64_t(pos) * 100 / size) : 0;
    r.push_back(String(p == bookPath ? "\x95 " : "   ") + bookName(p) + "   " + pct + "%");
  }
  return r;
}

void showScreen(Mode mode) {
  switch (screen) {
    case Screen::Reading:
      renderPage();
      break;
    case Screen::Menu:
      drawList(book.title(), menuRows(), sel, "Confirm select  Back close");
      break;
    case Screen::Contents:
      drawList("Contents", contentsRows(), sel, "Confirm go  Back menu");
      break;
    case Screen::Books:
      drawList("Books", booksRows(), sel, "Confirm open  Back menu");
      break;
  }
  if (mode == Mode::Fast) refreshTurn();
  else epd.refresh(canvas.getBuffer(), mode);
  if (screen == Screen::Reading) savePosition();
  lastActivity = millis();
}

int listLength() {
  switch (screen) {
    case Screen::Menu: return menuRows().size();
    case Screen::Contents: return max<int>(1, book.chapters().size());
    case Screen::Books: return bookPaths.size();
    default: return 0;
  }
}

// ------------------------------------------------------------ books / index ---

void setFont(int i) {
  fontIndex = i;
  LAYOUT.font = FONTS[i].font;
}

// A short notice drawn with a FAST refresh (e.g. "Indexing...").
void notice(const String& line) {
  canvas.setRotation(3);
  canvas.fillScreen(WHITE);
  canvas.setTextColor(BLACK);
  canvas.setFont(&NotoSerif28);
  canvas.setCursor(28, PH / 2);
  canvas.print(fitText(&NotoSerif28, line, PW - 56));
  epd.refresh(canvas.getBuffer(), Mode::Fast);
}

// Paginate the book for the current font: load "<book>.<px>.idx" or build + save it.
int indexBook() {
  const uint32_t t = millis();
  const String cachePath = bookPath + "." + FONTS[fontIndex].px + ".idx";
  const bool cached = book.loadIndex(SD, cachePath.c_str(), LAYOUT);
  if (!cached) {
    if (timelineSaved) notice("Indexing " + bookName(bookPath) + "\x85");  // not on the boot path
    book.buildIndex(LAYOUT);
    const bool saved = book.saveIndex(SD, cachePath.c_str(), LAYOUT);
    Serial.printf("index cache %s -> %s\n", saved ? "written" : "WRITE FAILED", cachePath.c_str());
  }
  Serial.printf("%s @ %d px: \"%s\" -> %d pages, %d chapters, %s in %lu ms, free heap %lu\n", bookPath.c_str(),
                FONTS[fontIndex].px, book.title().c_str(), book.pageCount(), (int)book.chapters().size(),
                cached ? "loaded from cache" : "indexed", (unsigned long)(millis() - t),
                (unsigned long)ESP.getFreeHeap());
  return book.pageCount();
}

// All *.txt in /books (or in the root if /books has none), sorted by name.
void findBooks() {
  bookPaths.clear();
  for (const char* dirName : {"/books", "/"}) {
    File dir = SD.open(dirName);
    if (!dir || !dir.isDirectory()) continue;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      const String name = f.name();
      if (!f.isDirectory() && (name.endsWith(".txt") || name.endsWith(".TXT"))) bookPaths.push_back(f.path());
    }
    if (!bookPaths.empty()) break;
  }
  std::sort(bookPaths.begin(), bookPaths.end());
  Serial.printf("books found: %d\n", (int)bookPaths.size());
  for (const String& p : bookPaths) Serial.printf("  %s\n", p.c_str());
}

// Open a book and go to its saved position.
bool openBook(const String& path) {
  if (!book.open(SD, path.c_str())) return false;
  bookPath = path;
  prefs.putString("book", bookPath);
  indexBook();
  page = constrain(book.pageForOffset(prefs.getUInt(posKey(bookPath).c_str(), 0)), 0, book.pageCount() - 1);
  pagesUntilClean = CLEAN_EVERY;
  return book.pageCount() > 0;
}

// ------------------------------------------------------------ bench, sleep ---

void message(const char* a, const char* b) {
  canvas.setRotation(3);
  canvas.fillScreen(WHITE);
  canvas.setTextColor(BLACK);
  canvas.setFont(&FreeSerif12pt7b);
  canvas.setCursor(28, 120);
  canvas.print(a);
  canvas.setCursor(28, 160);
  canvas.print(b);
  epd.refresh(canvas.getBuffer(), Mode::Full);
  epd.powerOff();
  Serial.printf("%s %s\n", a, b);
}

// Text screen for the bench (portrait, small font), one HALF refresh.
void benchScreen(const char* title, const String& body) {
  canvas.setRotation(3);
  canvas.fillScreen(WHITE);
  canvas.setTextColor(BLACK);
  canvas.setFont(&FreeSerif12pt7b);
  canvas.setCursor(28, 60);
  canvas.print(title);
  canvas.setFont(&FreeSans9pt7b);
  int y = 110, from = 0;
  while (from < (int)body.length()) {  // one line per '\n'
    int nl = body.indexOf('\n', from);
    if (nl < 0) nl = body.length();
    canvas.setCursor(28, y);
    canvas.print(body.substring(from, nl));
    y += 26;
    from = nl + 1;
  }
  epd.refresh(canvas.getBuffer(), Mode::Half);
  epd.powerOff();  // rails off: the panel must not skew the measurement
}

void runBench() {
  if (HWCDC::isPlugged()) {  // a computer is on the USB bus: light sleep would break it (G17e)
    benchScreen("Power bench",
                "Unplug the USB cable first:\nthe bench measures the battery,\nand light sleep would drop the\nUSB "
                "link.\n\nPress any page key to return.");
    return;
  }
  benchScreen("Power bench running", "About 2.5 minutes. Don't touch.\n6 modes x 20 s; the battery gauge\nmeasures each one.");
  power_bench::Result r[power_bench::NUM_MODES];
  // A lambda with no captures converts to a plain function pointer.
  power_bench::run(r, [](bool on) {
    if (on) epd.powerOn();
    else epd.powerOff();
  });

  String body, log;
  for (const auto& x : r) {
    char line[80];
    snprintf(line, sizeof(line), "%-18s %5.1f mA (%d..%d, n=%lu)", x.name, x.avgMa, x.minMa, x.maxMa,
             (unsigned long)x.samples);
    body += String(line) + "\n";
    log += String(line) + "; ";
  }
  batt = battery::read();
  body += "\nBattery now " + String(batt.percent) + "%, " + String(batt.mV) + " mV";
  body += "\n\nPress any page key to return.";
  prefs.putString("bench", log);
  benchScreen("Power bench results", body);
}

// Power off (battery) / deep sleep (USB). Never returns.
void goToSleep(const char* why) {
  batt = battery::read();
  Serial.printf("[%7lu] sleep (%s): batt %u%% %u mV %d mA\n", (unsigned long)millis(), why, batt.percent, batt.mV,
                batt.mA);

  // G19d: if the Power key is still down, waking on "Power LOW" would fire at once.
  pinMode(pins::BTN_POWER, INPUT_PULLUP);
  while (digitalRead(pins::BTN_POWER) == LOW) delay(10);

  // Leave the current page on the glass with a small banner (e-paper keeps it at 0 W).
  if (book.pageCount() > 0) {
    screen = Screen::Reading;
    renderPage();
    // Keep the clean page (no banner) in flash: the next wake can show it
    // before the SD card has started.
    const uint32_t t = millis();
    const bool ok = wake_frame::save(canvas.getBuffer(), FB_LEN, frameMeta(bookPath, book.pageStart(page)));
    prefs.putString("frameSave", String(ok ? "ok " : "FAILED ") + (millis() - t) + " ms");
    canvas.fillRect(28, PH - 80, PW - 56, 44, WHITE);
    canvas.drawRect(28, PH - 80, PW - 56, 44, BLACK);
    canvas.setFont(UI_FONT);
    canvas.setCursor(44, PH - 50);
    canvas.print("Sleeping \x97 press Power to continue");  // 0x97 = em dash
    epd.refresh(canvas.getBuffer(), Mode::Fast);
    savePosition();
  }
  epd.deepSleep();
  SD.end();
  prefs.putBool("cleanSleep", true);  // the glass now shows a known, clean frame
  prefs.end();
  Serial.flush();

  // Cut the battery/SD rail and keep the level through sleep (G19c).
  digitalWrite(pins::SD_POWER, LOW);
  gpio_hold_en(static_cast<gpio_num_t>(pins::SD_POWER));
  // Keep the panel's RESET high so the controller stays in deep sleep (USB case).
  gpio_hold_en(static_cast<gpio_num_t>(pins::EPD_RST));

  // Wake source for the USB-powered case: Power key (GPIO3) pulled LOW.
  esp_deep_sleep_enable_gpio_wakeup(1ULL << pins::BTN_POWER, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_start();  // on battery we never get here: power is already gone
}

// ------------------------------------------------------------- key handling ---

// What the queued keys asked for, gathered so one batch gives one refresh.
struct Actions {
  int pageDelta = 0;
  int selDelta = 0;
  bool select = false, back = false, sleep = false, bench = false;
};

void collect(Actions& a) {
  while (qHead != qTail) {
    const KeyEvent k = keyQueue[qHead];
    qHead = (qHead + 1) % 16;
    const bool tap = k.e == ButtonEvent::Released && k.held < Buttons::LONG_PRESS_MS;
    const bool fwd = k.b == Button::Right || k.b == Button::Down;
    const bool rev = k.b == Button::Left || k.b == Button::Up;

    if (k.e == ButtonEvent::LongPress && k.b == Button::Power) a.sleep = true;
    if (screen == Screen::Reading) {
      if (k.e == ButtonEvent::Pressed && fwd) a.pageDelta++;
      if (k.e == ButtonEvent::Pressed && rev) a.pageDelta--;
      if (k.e == ButtonEvent::LongPress && k.b == Button::Right) a.pageDelta += 9;  // + the press = 10
      if (k.e == ButtonEvent::LongPress && k.b == Button::Left) a.pageDelta -= 9;
      if (k.e == ButtonEvent::LongPress && k.b == Button::Confirm) a.bench = true;
      if (tap && k.b == Button::Back) a.back = true;  // = open the menu
    } else {
      if (k.e == ButtonEvent::Pressed && fwd) a.selDelta++;
      if (k.e == ButtonEvent::Pressed && rev) a.selDelta--;
      if (tap && k.b == Button::Confirm) a.select = true;
      if (tap && k.b == Button::Back) a.back = true;
    }
  }
}

void enter(Screen s, int selected) {
  screen = s;
  sel = selected;
  showScreen(Mode::Fast);
}

void handle(const Actions& a) {
  if (a.sleep) goToSleep("Power held");
  if (a.bench) {
    runBench();
    lastActivity = millis();
    return;
  }

  if (screen == Screen::Reading) {
    if (a.back) {
      enter(Screen::Menu, 0);
    } else if (a.pageDelta != 0) {
      const int target = constrain(page + a.pageDelta, 0, book.pageCount() - 1);
      if (target != page) {
        page = target;
        showScreen(Mode::Fast);
      }
    }
    return;
  }

  // In a menu or list
  if (a.back) {
    if (screen == Screen::Menu) {
      screen = Screen::Reading;
      showScreen(Mode::Half);  // back to the page: clean, no flash
    } else {
      enter(Screen::Menu, screen == Screen::Contents ? 0 : 1);
    }
    return;
  }
  if (a.select) {
    if (screen == Screen::Menu) {
      switch (sel) {
        case 0: enter(Screen::Contents, max(0, book.chapterForPage(page))); break;
        case 1: {
          const auto it = std::find(bookPaths.begin(), bookPaths.end(), bookPath);
          enter(Screen::Books, it == bookPaths.end() ? 0 : int(it - bookPaths.begin()));
          break;
        }
        case 2: {  // next text size, keep the first word on screen
          const uint32_t pos = book.pageStart(page);
          setFont((fontIndex + 1) % NUM_FONTS);
          prefs.putUChar("font", fontIndex);
          indexBook();
          page = book.pageForOffset(pos);
          showScreen(Mode::Fast);  // menu again, with the new size shown
          break;
        }
        case 3:
          screen = Screen::Reading;
          pagesUntilClean = CLEAN_EVERY;
          showScreen(Mode::Resync);
          break;
      }
    } else if (screen == Screen::Contents) {
      if (!book.chapters().empty()) {
        page = book.pageForOffset(book.chapters()[sel].offset);
        screen = Screen::Reading;
        showScreen(Mode::Half);
      }
    } else if (screen == Screen::Books) {
      if (bookPaths[sel] != bookPath) {
        savePosition();
        openBook(bookPaths[sel]);
      }
      screen = Screen::Reading;
      showScreen(Mode::Half);
    }
    return;
  }
  if (a.selDelta != 0) {
    const int n = listLength();
    const int s = constrain(sel + a.selDelta, 0, n - 1);
    if (s != sel) {
      sel = s;
      showScreen(Mode::Fast);
    }
  }
}

// ------------------------------------------------------------ setup / loop ---

void setup() {
  // FIRST: hold the battery latch (GPIO13). On a Power-key boot from battery the
  // rail only stays up while the key is pressed until firmware asserts this.
  gpio_hold_dis(static_cast<gpio_num_t>(pins::SD_POWER));  // undo the sleep hold
  gpio_hold_dis(static_cast<gpio_num_t>(pins::EPD_RST));
  gpio_deep_sleep_hold_dis();
  pinMode(pins::SD_POWER, OUTPUT);
  digitalWrite(pins::SD_POWER, HIGH);

  bootRomMs = esp_rtc_get_time_us() / 1000;
  mark("setup");
  const esp_reset_reason_t resetReason = esp_reset_reason();
  const bool wakeBoot = (resetReason == ESP_RST_POWERON || resetReason == ESP_RST_DEEPSLEEP);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  // Wait for a serial host only in the development case (upload/USB reset).
  if (!wakeBoot) while (!Serial && millis() < 1000) delay(10);
  mark("serial");
  if (bootMagic != 0xB007C0DE) {
    bootMagic = 0xB007C0DE;
    bootCount = 0;
  }
  bootCount++;
  Serial.printf("\n=== Xteink X3 stage 10: reader === boot #%lu, reset reason %d, wake cause %d\n",
                (unsigned long)bootCount, (int)resetReason, (int)esp_sleep_get_wakeup_cause());
  // NVS first: a clean wake needs the book/position/font to validate the saved frame.
  prefs.begin("reader", false);
  Serial.printf("previous wake timeline: %s\n", prefs.getString("wake", "(none)").c_str());
  Serial.printf("last frame save at sleep: %s\n", prefs.getString("frameSave", "(none)").c_str());
  setFont(constrain(prefs.getUChar("font", 0), 0, NUM_FONTS - 1));
  const bool cleanWake = wakeBoot && prefs.getBool("cleanSleep", false);
  if (prefs.getBool("cleanSleep", false)) prefs.putBool("cleanSleep", false);
  const String lastBook = prefs.getString("book", "");

  battery::begin();
  batt = battery::read();
  buttons.begin(onButton);
  mark("gauge+keys");
  epd.begin();  // display first: it owns the SPI bus
  epd.setIdleHook(whileRefreshing);
  mark("display");

  // Fast wake: show the page saved at sleep NOW, and let the SD card start up
  // (~650 ms cold) while the panel draws it (~450 ms waveform, CPU/bus free).
  bool frameStarted = false;
  if (cleanWake && !lastBook.isEmpty() &&
      wake_frame::load(canvas.getBuffer(), FB_LEN, frameMeta(lastBook, prefs.getUInt(posKey(lastBook).c_str(), 0)))) {
    epd.refreshStart(canvas.getBuffer(), Mode::Half);
    frameStarted = true;
    mark("frame started");
  }

  // Timed separately: is a slow cold mount the card itself, or a failed 25 MHz try?
  bool sdOk = SD.begin(pins::SD_CS, SPI, 25000000);
  mark(sdOk ? "sd@25M ok" : "sd@25M FAIL");
  if (!sdOk) {
    SD.end();
    sdOk = SD.begin(pins::SD_CS, SPI, 4000000);
    mark(sdOk ? "sd@4M ok" : "sd@4M FAIL");
  }
  if (!sdOk) {
    epd.refreshFinish();
    message("No SD card found.", "Insert a FAT32 card and reset.");
    return;
  }

  findBooks();
  if (bookPaths.empty()) {
    epd.refreshFinish();
    message("No .txt book found.", "Copy one into /books on the card.");
    return;
  }
  mark("find");
  // Reopen the last book if it's still on the card, else the first one.
  String last = lastBook;
  if (std::find(bookPaths.begin(), bookPaths.end(), last) == bookPaths.end()) last = bookPaths[0];
  const bool opened = openBook(last);
  mark("index");

  if (frameStarted) {
    epd.refreshFinish();  // the saved page is on the glass; usually already done
    char info[48];
    snprintf(info, sizeof(info), "saved frame, HALF %lu ms", (unsigned long)epd.stats.totalMs);
    saveTimeline(info);
    showBootInfo = false;
    lastActivity = millis();
    if (opened && last == lastBook) return;  // the frame is exactly this page
  }
  if (!opened) {
    message("Could not open the book.", last.c_str());
    return;
  }
  // Woke from OUR sleep: the glass shows a clean page + banner, so HALF is enough.
  showPage(cleanWake ? Mode::Half : Mode::Resync);
}

void loop() {
  buttons.update();
  if (book.pageCount() == 0) {
    delay(10);
    return;
  }

  Actions a;
  collect(a);
  handle(a);

  if (millis() - lastActivity > SLEEP_AFTER_MS) {
    goToSleep("idle");
  } else if (epd.powered() && millis() - lastActivity > RAILS_OFF_AFTER_MS) {
    epd.powerOff();
    Serial.printf("[%7lu] idle: panel rails off\n", (unsigned long)millis());
  }

  // Idle: light sleep instead of spinning, unless a computer is on the USB bus
  // (light sleep would drop the link, G17e) or a key is being handled.
  const bool idle = !buttons.busy() && millis() - lastKeyEvent > STAY_AWAKE_AFTER_KEY_MS && qHead == qTail;
  if (idle && !HWCDC::isPlugged()) power_bench::lightSleep(LIGHT_SLEEP_MS);
  else delay(1);
}
