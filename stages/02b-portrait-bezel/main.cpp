// Stage 2b — portrait orientation, bezel measurement, and a reboot detector.
//
// Questions this one refresh answers (look at the glass):
//   arrow         -> does setRotation(3) put "up" at the power button?
//   edge rulers   -> marks at 0,2,..,14 px from each edge: the smallest number
//                    you can see on a side = pixels hidden by the bezel there
//   boot line     -> after unplugging + replugging the magnetic cable:
//                    "boot #1, POWER-ON" = power was cut (hypothesis A)
//                    "boot #2+, other"   = chip stayed powered but reset (B)

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <esp_system.h>

#include "board_pins.h"
#include "epd_uc8253.h"

GFXcanvas1 canvas(EpdUc8253::WIDTH, EpdUc8253::HEIGHT);  // 792x528 physical
EpdUc8253 epd;

constexpr uint16_t WHITE = 1;
constexpr uint16_t BLACK = 0;

// RTC_NOINIT_ATTR: this RAM survives a reset but is lost when power is cut.
// (Python has no equivalent: think "a global that outlives a reboot".)
RTC_NOINIT_ATTR uint32_t bootMagic;
RTC_NOINIT_ATTR uint32_t bootCount;
constexpr uint32_t BOOT_MAGIC = 0xB007C0DE;

const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "POWER-ON";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT-WDT";
    case ESP_RST_TASK_WDT: return "TASK-WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    default: return "OTHER";
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

// Marks 2 px thick, parallel to each edge, at distance d = 0,2,..,14 px.
// The number is printed 34 px further in, where the bezel can't hide it.
void drawEdgeRulers() {
  const int W = canvas.width(), H = canvas.height();
  canvas.setFont(nullptr);  // built-in 6x8 font...
  canvas.setTextSize(2);    // ...scaled to 12x16
  for (int i = 0; i < 8; i++) {
    const int d = 2 * i;
    const int a = 40 + i * 30;   // position along the top/bottom edge
    const int b = 160 + i * 30;  // position along the left/right edge
    char label[4];
    snprintf(label, sizeof(label), "%d", d);

    canvas.fillRect(a, d, 24, 2, BLACK);  // top
    canvas.setCursor(a + 2, 34);
    canvas.print(label);

    canvas.fillRect(a, H - d - 2, 24, 2, BLACK);  // bottom
    canvas.setCursor(a + 2, H - 50);
    canvas.print(label);

    canvas.fillRect(d, b, 2, 24, BLACK);  // left
    canvas.setCursor(34, b + 4);
    canvas.print(label);

    canvas.fillRect(W - d - 2, b, 2, 24, BLACK);  // right
    canvas.setCursor(W - 60, b + 4);
    canvas.print(label);
  }
  canvas.setTextSize(1);
}

void drawScreen(uint32_t boot, const char* reason) {
  canvas.setRotation(3);  // prediction: portrait, power button at the top
  const int W = canvas.width();  // 528 after rotation
  canvas.fillScreen(WHITE);
  canvas.setTextColor(BLACK);

  drawEdgeRulers();

  // Arrow pointing at the top edge
  canvas.fillTriangle(W / 2, 90, W / 2 - 50, 170, W / 2 + 50, 170, BLACK);
  canvas.fillRect(W / 2 - 15, 170, 30, 70, BLACK);
  centeredText("Power button", 280, &FreeSans12pt7b);
  centeredText("should be up here", 310, &FreeSans12pt7b);

  centeredText("Portrait", 400, &FreeSansBold18pt7b);
  centeredText("setRotation(3)", 440, &FreeSans12pt7b);

  char line[48];
  snprintf(line, sizeof(line), "boot #%lu", (unsigned long)boot);
  centeredText(line, 540, &FreeSansBold18pt7b);
  snprintf(line, sizeof(line), "reset: %s", reason);
  centeredText(line, 580, &FreeSans12pt7b);

  centeredText("Ruler: smallest number", 650, &FreeSans12pt7b);
  centeredText("visible on each edge", 680, &FreeSans12pt7b);
  centeredText("= px hidden by bezel", 710, &FreeSans12pt7b);
}

void setup() {
  // Count boots that happen without losing power
  if (bootMagic != BOOT_MAGIC) {
    bootMagic = BOOT_MAGIC;
    bootCount = 0;
  }
  bootCount++;
  const esp_reset_reason_t reason = esp_reset_reason();

  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  while (!Serial && millis() < 4000) delay(10);
  delay(500);
  Serial.println("\n=== Xteink X3 stage 2b: portrait + bezel + reboot detector ===");
  Serial.printf("boot #%lu, reset reason: %s\n", (unsigned long)bootCount, resetReasonName(reason));

  pinMode(pins::SD_POWER, OUTPUT);
  digitalWrite(pins::SD_POWER, HIGH);

  drawScreen(bootCount, resetReasonName(reason));
  epd.begin();
  epd.displayFull(canvas.getBuffer());
  Serial.printf("refresh: %lu ms (power-on %lu, power-off %lu)\n", (unsigned long)epd.tRefresh.lowMs,
                (unsigned long)epd.tPowerOn.lowMs, (unsigned long)epd.tPowerOff.lowMs);
}

void loop() {
  // Heartbeat: if the serial log stops and restarts at boot #1, power was cut.
  static uint32_t last = 0;
  if (millis() - last >= 2000) {
    last = millis();
    Serial.printf("alive: boot #%lu, up %lu s\n", (unsigned long)bootCount, (unsigned long)(millis() / 1000));
  }
  delay(10);
}
