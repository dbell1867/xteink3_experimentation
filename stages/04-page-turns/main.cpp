// Stage 4 — faster page turns.
//
// A tiny "book" to compare refresh modes on the glass:
//   Right / Down  -> next page        (current page-turn mode)
//   Left  / Up    -> previous page
//   Confirm       -> switch page-turn mode FAST <-> NORMAL (redraws this page)
//   Back          -> HALF refresh of this page (clean-up)
//   Power (hold)  -> FULL refresh of this page
// Each page has text plus a black square that jumps around, so ghosting
// (faint leftovers of the previous page) is easy to spot.
// Serial prints the timing of every refresh; the footer shows the previous one.

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSerif12pt7b.h>

#include "board_pins.h"
#include "buttons.h"
#include "epd_uc8253.h"

using Mode = EpdUc8253::Mode;  // short alias (Python: Mode = EpdUc8253.Mode)

GFXcanvas1 canvas(EpdUc8253::WIDTH, EpdUc8253::HEIGHT);
EpdUc8253 epd;
Buttons buttons;

constexpr uint16_t WHITE = 1;
constexpr uint16_t BLACK = 0;
constexpr uint32_t RAILS_OFF_AFTER_MS = 30000;  // power the panel down when idle

const char* const PARAGRAPHS[] = {
    "It was a bright cold day in April, and the clocks were striking thirteen.",
    "Call me Ishmael. Some years ago, never mind how long precisely, having little money in my purse, "
    "I thought I would sail about a little and see the watery part of the world.",
    "It is a truth universally acknowledged, that a single man in possession of a good fortune, "
    "must be in want of a wife.",
    "All happy families are alike; each unhappy family is unhappy in its own way.",
    "The sky above the port was the color of television, tuned to a dead channel.",
    "In a hole in the ground there lived a hobbit. Not a nasty, dirty, wet hole, filled with the ends of "
    "worms and an oozy smell, nor yet a dry, bare, sandy hole.",
    "Happy families are all alike, but e-paper ghosts are each visible in their own way.",
};
constexpr int NUM_PARAGRAPHS = sizeof(PARAGRAPHS) / sizeof(PARAGRAPHS[0]);

// State
int page = 1;
Mode turnMode = Mode::Fast;
uint32_t lastActivity = 0;
uint32_t turnsSinceFull = 0;

// Actions queued by button presses, done by loop() (never inside a refresh).
int pendingPageDelta = 0;
bool pendingModeSwitch = false;
bool pendingHalf = false;
bool pendingFull = true;  // first screen

void onButton(Button b, ButtonEvent e, uint32_t) {
  if (e == ButtonEvent::Pressed) {
    switch (b) {
      case Button::Right:
      case Button::Down: pendingPageDelta++; break;
      case Button::Left:
      case Button::Up: pendingPageDelta--; break;
      case Button::Confirm: pendingModeSwitch = true; break;
      case Button::Back: pendingHalf = true; break;
      default: break;
    }
  } else if (e == ButtonEvent::LongPress && b == Button::Power) {
    pendingFull = true;
  }
}

// Draw text with simple word wrap inside [x, x+w); returns the next baseline y.
int drawWrapped(const char* text, int x, int y, int w, int lineH) {
  char word[48];
  int cx = x;
  const char* p = text;
  while (*p) {
    int n = 0;
    while (p[n] && p[n] != ' ' && n < 46) n++;
    memcpy(word, p, n);
    word[n] = ' ';
    word[n + 1] = 0;
    int16_t bx, by;
    uint16_t bw, bh;
    canvas.getTextBounds(word, 0, 0, &bx, &by, &bw, &bh);
    if (cx + bw > x + w && cx > x) {
      cx = x;
      y += lineH;
    }
    canvas.setCursor(cx, y);
    canvas.print(word);
    cx = canvas.getCursorX();
    p += n;
    while (*p == ' ') p++;
  }
  return y + lineH;
}

void drawPage(const EpdUc8253::Stats& prev) {
  canvas.setRotation(3);  // portrait, power button on top
  const int W = canvas.width(), H = canvas.height();
  canvas.fillScreen(WHITE);
  canvas.setTextColor(BLACK);

  // Header
  canvas.setFont(&FreeSansBold18pt7b);
  canvas.setCursor(24, 52);
  canvas.printf("Page %d", page);
  canvas.setFont(&FreeSans9pt7b);
  canvas.setCursor(W - 170, 50);
  canvas.printf("turn mode: %s", EpdUc8253::modeName(turnMode));
  canvas.drawFastHLine(24, 68, W - 48, BLACK);

  // Body text: three paragraphs, starting point depends on the page
  canvas.setFont(&FreeSerif12pt7b);
  int y = 105;
  for (int i = 0; i < 3; i++) {
    y = drawWrapped(PARAGRAPHS[(page * 2 + i) % NUM_PARAGRAPHS], 24, y, W - 48, 30) + 10;
  }

  // Ghost target: a 120 px black square in one of 4 spots
  const int spot = page % 4;
  const int sx = (spot % 2) ? W - 24 - 120 : 24;
  const int sy = 470 + (spot / 2) * 140;
  canvas.fillRect(sx, sy, 120, 120, BLACK);

  // Footer: timing of the previous refresh
  canvas.drawFastHLine(24, H - 70, W - 48, BLACK);
  canvas.setFont(&FreeSans9pt7b);
  canvas.setCursor(24, H - 46);
  canvas.printf("prev: %s%s%s  %lu ms (wave %lu)", EpdUc8253::modeName(prev.used), prev.why[0] ? " <- " : "",
                prev.why[0] ? EpdUc8253::modeName(prev.requested) : "", (unsigned long)prev.totalMs,
                (unsigned long)prev.waveformMs);
  canvas.setCursor(24, H - 22);
  canvas.printf("turns since FULL: %lu   R/D next  L/U prev", (unsigned long)turnsSinceFull);
}

void show(Mode mode) {
  drawPage(epd.stats);
  epd.refresh(canvas.getBuffer(), mode);
  const auto& s = epd.stats;
  if (s.used == Mode::Full) turnsSinceFull = 0;
  else turnsSinceFull++;
  Serial.printf("[%7lu] page %3d  %-6s%s%-6s  total %4lu ms = upload %3lu + power-on %3lu + waveform %4lu + extra %4lu%s%s\n",
                (unsigned long)millis(), page, EpdUc8253::modeName(s.used), s.why[0] ? " (asked " : "",
                s.why[0] ? EpdUc8253::modeName(s.requested) : "", (unsigned long)s.totalMs,
                (unsigned long)s.uploadMs, (unsigned long)s.powerOnMs, (unsigned long)s.waveformMs,
                (unsigned long)s.extraMs, s.why[0] ? ", " : "", s.why[0] ? s.why : "");
  lastActivity = millis();
}

void whileRefreshing() { buttons.update(); }

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  while (!Serial && millis() < 4000) delay(10);
  delay(500);
  Serial.println("\n=== Xteink X3 stage 4: page turns ===");

  pinMode(pins::SD_POWER, OUTPUT);
  digitalWrite(pins::SD_POWER, HIGH);

  buttons.begin(onButton);
  epd.begin();
  epd.setIdleHook(whileRefreshing);
}

void loop() {
  buttons.update();

  if (pendingFull) {
    pendingFull = false;
    show(Mode::Full);
  } else if (pendingHalf) {
    pendingHalf = false;
    show(Mode::Half);
  } else if (pendingModeSwitch) {
    pendingModeSwitch = false;
    turnMode = (turnMode == Mode::Fast) ? Mode::Normal : Mode::Fast;
    Serial.printf("turn mode -> %s\n", EpdUc8253::modeName(turnMode));
    show(turnMode);
  } else if (pendingPageDelta != 0) {
    page += pendingPageDelta;  // presses during a refresh add up
    if (page < 1) page = 1;
    pendingPageDelta = 0;
    show(turnMode);
  } else if (epd.powered() && millis() - lastActivity > RAILS_OFF_AFTER_MS) {
    epd.powerOff();
    Serial.printf("[%7lu] idle: panel rails off\n", (unsigned long)millis());
  }
  delay(1);
}
