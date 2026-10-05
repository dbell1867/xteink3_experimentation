// Stage 7 — text reader + battery + power-off.
//
// Opens the first .txt in /books on the SD card, indexes its pages, and shows
// them with FAST turns + a RESYNC every 15 (stage 5's ghost policy).
//   Right / Down -> next page        Left / Up -> previous page
//   hold Right / Left -> jump 10 pages
//   Back tap -> HALF clean-up        Back hold -> RESYNC clean-up
//   hold Power -> sleep now (also after 5 min idle)
// The current page is saved in flash (NVS) so the book reopens where you were.
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
#include <esp_sleep.h>
#include <esp_system.h>

#include "battery.h"
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
constexpr uint32_t SLEEP_AFTER_MS = 5 * 60 * 1000;  // auto power-off when idle

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
bool pendingHalf = false, pendingResync = false, pendingSleep = false;
BatteryReading batt;
// Wake timeline: millis() at each boot phase (millis() starts when the app
// starts, so ROM + bootloader time before setup() is not included).
struct Mark { const char* name; uint32_t ms; };
Mark marks[8];
int numMarks = 0;
void mark(const char* name) {
  if (numMarks < 8) marks[numMarks++] = {name, millis()};
}
bool timelineSaved = false;

bool showBootInfo = true;  // first page after boot shows boot # / reset reason instead of the title

void onButton(Button b, ButtonEvent e, uint32_t heldMs) {
  if (e == ButtonEvent::Pressed) {
    if (b == Button::Right || b == Button::Down) pendingDelta++;
    if (b == Button::Left || b == Button::Up) pendingDelta--;
  } else if (e == ButtonEvent::LongPress) {
    if (b == Button::Right) pendingDelta += 9;  // + the 1 from the press = 10
    if (b == Button::Left) pendingDelta -= 9;
    if (b == Button::Back) pendingResync = true;
    if (b == Button::Power) pendingSleep = true;
  } else if (e == ButtonEvent::Released) {
    if (b == Button::Back && heldMs < Buttons::LONG_PRESS_MS) pendingHalf = true;
  }
}

void drawChrome() {
  canvas.setFont(&FreeSans9pt7b);
  canvas.setCursor(28, 32);
  if (showBootInfo)  // boot evidence, visible even on battery; time = up to this draw
    canvas.printf("#%lu r%d w%d  draw@%lums", (unsigned long)bootCount, (int)esp_reset_reason(),
                  (int)esp_sleep_get_wakeup_cause(), (unsigned long)millis());
  else
    canvas.print(book.title().substring(0, 26));
  // Right side: battery (+ = charging, mA from the gauge) and page number
  char pg[48];
  if (batt.ok)
    snprintf(pg, sizeof(pg), "%s%u%% %dmA  %d/%d", batt.mA > 0 ? "+" : "", batt.percent, batt.mA, page + 1,
             book.pageCount());
  else
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
  batt = battery::read();
  const uint32_t t0 = millis();
  book.drawPage(canvas, LAYOUT, page);
  drawChrome();
  const uint32_t layoutMs = millis() - t0;
  epd.refresh(canvas.getBuffer(), mode);
  Serial.printf("[%7lu] page %d/%d  layout %lu ms + %s %lu ms   batt %u%% %u mV %d mA\n", (unsigned long)millis(),
                page + 1, book.pageCount(), (unsigned long)layoutMs, EpdUc8253::modeName(epd.stats.used),
                (unsigned long)epd.stats.totalMs, batt.percent, batt.mV, batt.mA);
  prefs.putInt("page", page);
  if (!timelineSaved) {
    // First page after boot: record the whole timeline (incl. the refresh) in NVS.
    mark("page shown");
    String t;
    for (int i = 0; i < numMarks; i++) t += String(marks[i].name) + "=" + marks[i].ms + " ";
    t += "(" + String(EpdUc8253::modeName(epd.stats.used)) + " " + epd.stats.totalMs + " ms)";
    prefs.putString("wake", t);
    Serial.printf("wake timeline: %s\n", t.c_str());
    timelineSaved = true;
  }
  showBootInfo = false;
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
    canvas.setRotation(3);
    canvas.fillScreen(WHITE);
    canvas.setTextColor(BLACK);
    book.drawPage(canvas, LAYOUT, page);
    drawChrome();
    canvas.fillRect(28, PH - 80, PW - 56, 44, WHITE);
    canvas.drawRect(28, PH - 80, PW - 56, 44, BLACK);
    canvas.setFont(&FreeSans9pt7b);
    canvas.setCursor(44, PH - 52);
    canvas.print("Sleeping - press Power to continue");
    epd.refresh(canvas.getBuffer(), Mode::Fast);
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

void setup() {
  // FIRST: hold the battery latch (GPIO13). On a Power-key boot from battery the
  // rail only stays up while the key is pressed until firmware asserts this.
  gpio_hold_dis(static_cast<gpio_num_t>(pins::SD_POWER));  // undo the sleep hold
  gpio_hold_dis(static_cast<gpio_num_t>(pins::EPD_RST));
  gpio_deep_sleep_hold_dis();
  pinMode(pins::SD_POWER, OUTPUT);
  digitalWrite(pins::SD_POWER, HIGH);

  mark("setup");
  const esp_reset_reason_t resetReason = esp_reset_reason();
  const bool wakeBoot = (resetReason == ESP_RST_POWERON || resetReason == ESP_RST_DEEPSLEEP);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  // Wait for a serial host only in the development case (upload/USB reset).
  // On a Power-key wake no host is there in time anyway: that wait cost 990 ms.
  if (!wakeBoot) while (!Serial && millis() < 1000) delay(10);
  mark("serial");
  if (bootMagic != 0xB007C0DE) {
    bootMagic = 0xB007C0DE;
    bootCount = 0;
  }
  bootCount++;
  Serial.printf("\n=== Xteink X3 stage 7: reader === boot #%lu, reset reason %d, wake cause %d\n",
                (unsigned long)bootCount, (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause());
  delay(50);  // let the SD card's supply settle

  battery::begin();
  batt = battery::read();
  Serial.printf("battery: %s %u%% %u mV %d mA\n", batt.ok ? "ok" : "NO GAUGE", batt.percent, batt.mV, batt.mA);

  buttons.begin(onButton);
  epd.begin();  // display first: it owns the SPI bus
  epd.setIdleHook(whileRefreshing);

  mark("display");
  const bool sdOk = SD.begin(pins::SD_CS, SPI, 25000000) || SD.begin(pins::SD_CS, SPI, 4000000);
  mark("sd mount");
  if (!sdOk) {
    message("No SD card found.", "Insert a FAT32 card and reset.");
    return;
  }
  bookPath = findBook();
  if (bookPath.isEmpty() || !book.open(SD, bookPath.c_str())) {
    message("No .txt book found.", "Copy one into /books on the card.");
    return;
  }

  mark("find");
  const uint32_t t = millis();
  const String cachePath = bookPath + ".idx";
  int pages;
  const bool cached = book.loadIndex(SD, cachePath.c_str(), LAYOUT);
  if (cached) {
    pages = book.pageCount();
    mark("index cached");
  } else {
    pages = book.buildIndex(LAYOUT);
    const bool saved = book.saveIndex(SD, cachePath.c_str(), LAYOUT);
    mark("index built");
    Serial.printf("index cache %s -> %s\n", saved ? "written" : "WRITE FAILED", cachePath.c_str());
  }
  Serial.printf("%s: \"%s\" -> %d pages, %s in %lu ms, free heap %lu\n", bookPath.c_str(), book.title().c_str(), pages,
                cached ? "loaded from cache" : "indexed", (unsigned long)(millis() - t), (unsigned long)ESP.getFreeHeap());

  // Resume where we left off (only if it's the same book).
  prefs.begin("reader", false);
  Serial.printf("previous wake timeline: %s\n", prefs.getString("wake", "(none)").c_str());
  if (prefs.getString("book", "") == bookPath) page = prefs.getInt("page", 0);
  else prefs.putString("book", bookPath);
  page = constrain(page, 0, pages - 1);  // Python: min(max(page, 0), pages - 1)

  // Woke from OUR sleep: the glass shows a clean page + banner, so HALF (which
  // ignores the old frame) is enough. Anything else: full clean-up.
  const bool cleanWake = wakeBoot && prefs.getBool("cleanSleep", false);
  if (prefs.getBool("cleanSleep", false)) prefs.putBool("cleanSleep", false);
  Serial.printf("clean wake: %s\n", cleanWake ? "yes -> HALF" : "no -> RESYNC");
  showPage(cleanWake ? Mode::Half : Mode::Resync);
}

void loop() {
  buttons.update();
  if (book.pageCount() == 0) {
    delay(10);
    return;
  }

  if (pendingSleep) {
    goToSleep("Power held");
  } else if (millis() - lastActivity > SLEEP_AFTER_MS) {
    goToSleep("idle");
  } else if (pendingResync) {
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
