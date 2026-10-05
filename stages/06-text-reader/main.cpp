// Stage 6b — a plain-text book reader.
//
// Opens the first .txt in /books on the SD card, indexes its pages, and shows
// them with FAST turns + a RESYNC every 15 (stage 5's ghost policy).
//   Right / Down -> next page        Left / Up -> previous page
//   hold Right / Left -> jump 10 pages
//   Back tap -> HALF clean-up        Back hold -> RESYNC clean-up
// The current page is saved in flash (NVS) so the book reopens where you were.

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSerif12pt7b.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <esp_system.h>

#include "board_pins.h"
#include "buttons.h"
#include "epd_uc8253.h"
#include "text_book.h"

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

// Portrait page geometry (528 x 792; the bezel hides 3 px per side).
constexpr int PW = 528, PH = 792;
const TextBook::Layout LAYOUT = {
    &FreeSerif12pt7b,
    28, PW - 28,  // left, right
    58, PH - 44,  // top, bottom
    10,           // paragraph gap
    24,           // indent
};

// Reboot detector (survives a reset, not a power cut) - still hunting the
// occasional short USB drop + reboot.
RTC_NOINIT_ATTR uint32_t bootMagic;
RTC_NOINIT_ATTR uint32_t bootCount;

String bookPath;
int page = 0;
int pagesUntilClean = CLEAN_EVERY;
uint32_t lastActivity = 0;

int pendingDelta = 0;
bool pendingHalf = false, pendingResync = false;

void onButton(Button b, ButtonEvent e, uint32_t heldMs) {
  if (e == ButtonEvent::Pressed) {
    if (b == Button::Right || b == Button::Down) pendingDelta++;
    if (b == Button::Left || b == Button::Up) pendingDelta--;
  } else if (e == ButtonEvent::LongPress) {
    if (b == Button::Right) pendingDelta += 9;  // + the 1 from the press = 10
    if (b == Button::Left) pendingDelta -= 9;
    if (b == Button::Back) pendingResync = true;
  } else if (e == ButtonEvent::Released) {
    if (b == Button::Back && heldMs < Buttons::LONG_PRESS_MS) pendingHalf = true;
  }
}

void drawChrome() {
  canvas.setFont(&FreeSans9pt7b);
  canvas.setCursor(28, 32);
  canvas.print(book.title());
  char pg[24];
  snprintf(pg, sizeof(pg), "%d / %d", page + 1, book.pageCount());
  int16_t x1, y1;
  uint16_t w, h;
  canvas.getTextBounds(pg, 0, 0, &x1, &y1, &w, &h);
  canvas.setCursor(PW - 28 - w, 32);
  canvas.print(pg);
  canvas.drawFastHLine(28, 42, PW - 56, BLACK);

  // Progress bar along the bottom
  const int barW = PW - 56;
  canvas.drawFastHLine(28, PH - 20, barW, BLACK);
  canvas.fillRect(28, PH - 22, barW * (page + 1) / book.pageCount(), 5, BLACK);
}

void showPage(Mode mode) {
  canvas.setRotation(3);
  canvas.fillScreen(WHITE);
  canvas.setTextColor(BLACK);
  const uint32_t t0 = millis();
  book.drawPage(canvas, LAYOUT, page);
  drawChrome();
  const uint32_t layoutMs = millis() - t0;
  epd.refresh(canvas.getBuffer(), mode);
  Serial.printf("[%7lu] page %d/%d  layout %lu ms + %s %lu ms\n", (unsigned long)millis(), page + 1,
                book.pageCount(), (unsigned long)layoutMs, EpdUc8253::modeName(epd.stats.used),
                (unsigned long)epd.stats.totalMs);
  prefs.putInt("page", page);
  lastActivity = millis();
}

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

// First *.txt in /books (or in the root if there is no /books).
// Logs every entry it looks at, so a "wrong book" is easy to diagnose.
String findBook() {
  String found;
  for (const char* dirName : {"/books", "/"}) {
    File dir = SD.open(dirName);
    if (!dir || !dir.isDirectory()) {
      Serial.printf("  %s: not found\n", dirName);
      continue;
    }
    Serial.printf("  %s:\n", dirName);
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      const String name = f.name();
      Serial.printf("    %s%s  %lu bytes\n", name.c_str(), f.isDirectory() ? "/" : "", (unsigned long)f.size());
      if (found.isEmpty() && !f.isDirectory() && (name.endsWith(".txt") || name.endsWith(".TXT")))
        found = f.path();
    }
    if (!found.isEmpty()) break;
  }
  return found;
}

void whileRefreshing() { buttons.update(); }

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  while (!Serial && millis() < 4000) delay(10);
  delay(500);
  if (bootMagic != 0xB007C0DE) {
    bootMagic = 0xB007C0DE;
    bootCount = 0;
  }
  bootCount++;
  Serial.printf("\n=== Xteink X3 stage 6b: text reader === boot #%lu, reset reason %d\n", (unsigned long)bootCount,
                (int)esp_reset_reason());

  pinMode(pins::SD_POWER, OUTPUT);
  digitalWrite(pins::SD_POWER, HIGH);
  delay(50);

  buttons.begin(onButton);
  epd.begin();  // display first: it owns the SPI bus
  epd.setIdleHook(whileRefreshing);

  if (!SD.begin(pins::SD_CS, SPI, 25000000) && !SD.begin(pins::SD_CS, SPI, 4000000)) {
    message("No SD card found.", "Insert a FAT32 card and reset.");
    return;
  }
  bookPath = findBook();
  if (bookPath.isEmpty() || !book.open(SD, bookPath.c_str())) {
    message("No .txt book found.", "Copy one into /books on the card.");
    return;
  }

  const uint32_t t = millis();
  const int pages = book.buildIndex(LAYOUT);
  Serial.printf("%s: \"%s\" -> %d pages, indexed in %lu ms, free heap %lu\n", bookPath.c_str(), book.title().c_str(),
                pages, (unsigned long)(millis() - t), (unsigned long)ESP.getFreeHeap());

  // Resume where we left off (only if it's the same book).
  prefs.begin("reader", false);
  if (prefs.getString("book", "") == bookPath) page = prefs.getInt("page", 0);
  else prefs.putString("book", bookPath);
  page = constrain(page, 0, pages - 1);  // Python: min(max(page, 0), pages - 1)

  showPage(Mode::Resync);
}

void loop() {
  buttons.update();
  if (book.pageCount() == 0) {
    delay(10);
    return;
  }

  if (pendingResync) {
    pendingResync = false;
    pagesUntilClean = CLEAN_EVERY;
    showPage(Mode::Resync);
  } else if (pendingHalf) {
    pendingHalf = false;
    showPage(Mode::Half);
  } else if (pendingDelta != 0) {
    const int target = constrain(page + pendingDelta, 0, book.pageCount() - 1);
    pendingDelta = 0;
    if (target != page) {
      page = target;
      Mode m = Mode::Fast;
      if (--pagesUntilClean <= 0) {
        m = Mode::Resync;
        pagesUntilClean = CLEAN_EVERY;
      }
      showPage(m);
    }
  } else if (epd.powered() && millis() - lastActivity > RAILS_OFF_AFTER_MS) {
    epd.powerOff();
    Serial.printf("[%7lu] idle: panel rails off\n", (unsigned long)millis());
  }
  delay(1);
}
