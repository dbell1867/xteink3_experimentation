// Stage 2 — first light on the Xteink X3 e-paper (UC8253, 792x528).
//
// Draw ONE test pattern into a RAM framebuffer, then show it with ONE full
// refresh (G44), timing every BUSY phase (G46). The pattern is a set of
// questions you answer by looking at the glass:
//   border        -> are all four edges on screen? (geometry / the 600-vs-528 question)
//   corner labels -> which way round is the image? (orientation / mirroring)
//   stripes       -> are 1-pixel lines sharp? (bit order, SPI integrity)
//   black block   -> is black solid and white clean? (waveform / polarity)

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

#include "board_pins.h"
#include "epd_uc8253.h"

// A 1-bit canvas is exactly the layout the controller wants: rows of 99 bytes,
// leftmost pixel in the top bit. Color 1 = white, 0 = black.
// (Python: like a bytearray(52272) with draw helpers attached.)
GFXcanvas1 canvas(EpdUc8253::WIDTH, EpdUc8253::HEIGHT);
EpdUc8253 epd;

constexpr uint16_t WHITE = 1;
constexpr uint16_t BLACK = 0;

void centeredText(const char* s, int16_t y, const GFXfont* font) {
  int16_t x1, y1;
  uint16_t w, h;
  canvas.setFont(font);
  canvas.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  canvas.setCursor((EpdUc8253::WIDTH - w) / 2 - x1, y);
  canvas.print(s);
}

void drawTestPattern() {
  const int W = EpdUc8253::WIDTH, H = EpdUc8253::HEIGHT;
  canvas.fillScreen(WHITE);
  canvas.setTextColor(BLACK);

  // Border: 3 px, flush with the edges
  for (int i = 0; i < 3; i++) canvas.drawRect(i, i, W - 2 * i, H - 2 * i, BLACK);

  // Corner labels
  canvas.setFont(&FreeSans12pt7b);
  canvas.setCursor(12, 32);
  canvas.print("TOP-LEFT");
  canvas.setCursor(W - 150, 32);
  canvas.print("TOP-RIGHT");
  canvas.setCursor(12, H - 14);
  canvas.print("BOTTOM-LEFT");
  canvas.setCursor(W - 190, H - 14);
  canvas.print("BOTTOM-RIGHT");

  // Title
  centeredText("Xteink X3 - first light", 120, &FreeSansBold18pt7b);
  centeredText("UC8253  792 x 528  FULL refresh", 165, &FreeSans12pt7b);

  // Vertical stripes: 1, 2 and 4 px wide, each in its own 120 px block
  const int sy = 210, sh = 120;
  int x = 60;
  for (int width : {1, 2, 4}) {
    for (int i = 0; i < 120; i += 2 * width) canvas.fillRect(x + i, sy, width, sh, BLACK);
    canvas.setFont(&FreeSans12pt7b);
    canvas.setCursor(x + 30, sy + sh + 28);
    canvas.printf("%d px", width);
    x += 170;
  }

  // Solid black block with white text inside
  canvas.fillRect(W - 230, sy, 170, sh, BLACK);
  canvas.setTextColor(WHITE);
  canvas.setCursor(W - 205, sy + 70);
  canvas.print("BLACK");
  canvas.setTextColor(BLACK);
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  while (!Serial && millis() < 4000) delay(10);
  delay(500);
  Serial.println("\n=== Xteink X3 stage 2: e-paper first light ===");

  pinMode(pins::SD_POWER, OUTPUT);  // keep the SD card powered but deselected
  digitalWrite(pins::SD_POWER, HIGH);

  if (canvas.getBuffer() == nullptr) {
    Serial.println("ERROR: could not allocate the 52 KB framebuffer");
    return;
  }
  Serial.printf("Framebuffer: %u bytes, free heap now %lu\n", EpdUc8253::ROW_BYTES * EpdUc8253::HEIGHT,
                (unsigned long)ESP.getFreeHeap());

  uint32_t t = millis();
  drawTestPattern();
  Serial.printf("Drawing into RAM:     %lu ms\n", (unsigned long)(millis() - t));

  t = millis();
  epd.begin();
  Serial.printf("Controller init:      %lu ms (BUSY now %d)\n", (unsigned long)(millis() - t),
                digitalRead(pins::EPD_BUSY));

  t = millis();
  epd.displayFull(canvas.getBuffer());
  const uint32_t total = millis() - t;

  auto show = [](const char* name, const EpdUc8253::BusyTiming& b) {
    Serial.printf("%-21s %s  start %lu ms, busy %lu ms\n", name, b.sawLow ? "ok  " : "NO-LOW", (unsigned long)b.toLowMs,
                  (unsigned long)b.lowMs);
  };
  Serial.printf("LUTs + image upload:  %lu ms\n", (unsigned long)epd.sendMs);
  show("Power on:", epd.tPowerOn);
  show("Refresh:", epd.tRefresh);
  show("Power off:", epd.tPowerOff);
  Serial.printf("displayFull total:    %lu ms\n", (unsigned long)total);
  Serial.println("Done. The image stays on the glass with no power.");
}

void loop() {
  delay(1000);
}
