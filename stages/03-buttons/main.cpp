// Stage 3 — buttons.
//
// Every button event goes to serial immediately (with a timestamp); the screen
// shows a tally per button and highlights the last one pressed. Buttons keep
// being sampled DURING the ~1.2 s e-paper refresh via the driver's idle hook,
// so presses made while the screen is flashing are still counted.
//
// Things to check:
//   - each key maps to the right name (front: Back/Confirm/Left/Right, side: Up/Down)
//   - one press = exactly one count (debounce works, no double counts)
//   - "glitches" on serial: raw readings rejected by the 20 ms stability rule
//   - holding a key > 0.7 s gives one LONG

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <esp_system.h>

#include "board_pins.h"
#include "buttons.h"
#include "epd_uc8253.h"

GFXcanvas1 canvas(EpdUc8253::WIDTH, EpdUc8253::HEIGHT);
EpdUc8253 epd;
Buttons buttons;

constexpr uint16_t WHITE = 1;
constexpr uint16_t BLACK = 0;

// Reboot detector from stage 2b (still hunting the one unexplained reboot)
RTC_NOINIT_ATTR uint32_t bootMagic;
RTC_NOINIT_ATTR uint32_t bootCount;
constexpr uint32_t BOOT_MAGIC = 0xB007C0DE;

// Tallies, indexed by Button (static_cast<int>(Button::Left) etc.)
uint32_t presses[8] = {};
uint32_t longs[8] = {};
Button lastButton = Button::None;
uint32_t lastHeldMs = 0;
uint32_t pressesDuringRefresh = 0;
bool refreshing = false;
bool screenDirty = true;  // draw once at boot
uint32_t refreshCount = 0, lastRefreshMs = 0;
const char* resetName = "?";

const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "POWER-ON";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT-WDT";
    case ESP_RST_TASK_WDT: return "TASK-WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_USB: return "USB";
    default: return "OTHER";
  }
}

// Called by Buttons for every accepted event.
void onButton(Button b, ButtonEvent e, uint32_t heldMs) {
  const int i = static_cast<int>(b);
  const char* what = e == ButtonEvent::Pressed ? "pressed" : e == ButtonEvent::LongPress ? "LONG" : "released";
  Serial.printf("[%7lu] %-8s %-8s held %4lu ms%s\n", (unsigned long)millis(), buttonName(b), what,
                (unsigned long)heldMs, refreshing ? "  (during refresh)" : "");

  if (e == ButtonEvent::Pressed) {
    presses[i]++;
    lastButton = b;
    if (refreshing) pressesDuringRefresh++;
    screenDirty = true;
  } else if (e == ButtonEvent::LongPress) {
    longs[i]++;
    screenDirty = true;
  } else {
    lastHeldMs = heldMs;
  }
}

void centeredText(const char* s, int16_t y, const GFXfont* font) {
  int16_t x1, y1;
  uint16_t w, h;
  canvas.setFont(font);
  canvas.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  canvas.setCursor((canvas.width() - w) / 2 - x1, y);
  canvas.print(s);
}

void drawScreen() {
  canvas.setRotation(3);  // portrait, power button on top
  const int W = canvas.width();
  canvas.fillScreen(WHITE);
  canvas.setTextColor(BLACK);

  centeredText("Stage 3: buttons", 50, &FreeSansBold18pt7b);
  centeredText("press any key", 85, &FreeSans12pt7b);

  // One row per button; the last one pressed is drawn inverted.
  const Button order[] = {Button::Back, Button::Confirm, Button::Left, Button::Right,
                          Button::Up,   Button::Down,    Button::Power};
  int y = 115;
  canvas.setFont(&FreeSans12pt7b);
  for (Button b : order) {
    const int i = static_cast<int>(b);
    const bool hi = (b == lastButton);
    if (hi)
      canvas.fillRect(20, y, W - 40, 56, BLACK);
    else
      canvas.drawRect(20, y, W - 40, 56, BLACK);
    canvas.setTextColor(hi ? WHITE : BLACK);
    canvas.setCursor(40, y + 37);
    canvas.print(buttonName(b));
    canvas.setCursor(W - 220, y + 37);
    canvas.printf("%3lu   long %lu", (unsigned long)presses[i], (unsigned long)longs[i]);
    y += 66;
  }
  canvas.setTextColor(BLACK);

  char line[64];
  snprintf(line, sizeof(line), "last: %s, held %lu ms", buttonName(lastButton), (unsigned long)lastHeldMs);
  centeredText(line, 610, &FreeSans12pt7b);
  snprintf(line, sizeof(line), "glitches %lu, during refresh %lu", (unsigned long)buttons.glitches,
           (unsigned long)pressesDuringRefresh);
  centeredText(line, 645, &FreeSans12pt7b);
  snprintf(line, sizeof(line), "refresh #%lu: %lu ms", (unsigned long)refreshCount, (unsigned long)lastRefreshMs);
  centeredText(line, 680, &FreeSans12pt7b);
  snprintf(line, sizeof(line), "boot #%lu, reset: %s", (unsigned long)bootCount, resetName);
  centeredText(line, 740, &FreeSans12pt7b);
}

// The driver calls this every ~1 ms while waiting on BUSY.
void whileRefreshing() { buttons.update(); }

void setup() {
  if (bootMagic != BOOT_MAGIC) {
    bootMagic = BOOT_MAGIC;
    bootCount = 0;
  }
  bootCount++;
  resetName = resetReasonName(esp_reset_reason());

  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  while (!Serial && millis() < 4000) delay(10);
  delay(500);
  Serial.println("\n=== Xteink X3 stage 3: buttons ===");
  Serial.printf("boot #%lu, reset reason: %s\n", (unsigned long)bootCount, resetName);

  pinMode(pins::SD_POWER, OUTPUT);
  digitalWrite(pins::SD_POWER, HIGH);

  buttons.begin(onButton);
  epd.begin();
  epd.setIdleHook(whileRefreshing);
}

void loop() {
  buttons.update();

  if (screenDirty) {
    screenDirty = false;  // presses during the refresh set it again -> one more redraw
    drawScreen();
    refreshing = true;
    const uint32_t t = millis();
    epd.displayFull(canvas.getBuffer());
    lastRefreshMs = millis() - t;
    refreshing = false;
    refreshCount++;
    Serial.printf("[%7lu] screen refresh #%lu took %lu ms\n", (unsigned long)millis(), (unsigned long)refreshCount,
                  (unsigned long)lastRefreshMs);
  }
  delay(1);
}
