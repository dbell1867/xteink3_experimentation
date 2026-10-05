// Stage 5 — ghost management.
//
// Page turns use FAST; every N turns the turn is replaced by a clean-up
// refresh (CrossPoint's scheme, default N = 15, RESYNC on the X3).
//   Right / Down  -> next page          Left / Up -> previous page
//   Confirm       -> cycle N: 1, 5, 10, 15, 30, off
//   Back (tap)    -> HALF clean-up of this page now
//   Back (hold)   -> RESYNC clean-up of this page now
//   Power (hold)  -> stress test: 20 automatic FAST turns, no clean-up
// The black square jumps between 4 spots and the text reflows each page, so
// ghosting (faint leftovers of earlier pages) builds up where you can see it.

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSerif12pt7b.h>

#include "board_pins.h"
#include "buttons.h"
#include "epd_uc8253.h"

using Mode = EpdUc8253::Mode;

GFXcanvas1 canvas(EpdUc8253::WIDTH, EpdUc8253::HEIGHT);
EpdUc8253 epd;
Buttons buttons;

constexpr uint16_t WHITE = 1;
constexpr uint16_t BLACK = 0;
constexpr uint32_t RAILS_OFF_AFTER_MS = 30000;
constexpr int STRESS_TURNS = 20;
constexpr int CLEAN_CHOICES[] = {1, 5, 10, 15, 30, 0};  // 0 = never
constexpr int NUM_CLEAN_CHOICES = sizeof(CLEAN_CHOICES) / sizeof(CLEAN_CHOICES[0]);
constexpr Mode PERIODIC_CLEAN = Mode::Resync;  // what CrossPoint does on the X3

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
int cleanChoice = 3;      // index into CLEAN_CHOICES -> 15
int pagesUntilClean = 15;  // counts down like CrossPoint's pagesUntilFullRefresh
uint32_t fastSinceClean = 0;
int stressRemaining = 0;
uint32_t lastActivity = 0;

// Actions queued by button presses, done by loop() (never inside a refresh).
int pendingPageDelta = 0;
bool pendingCycleN = false;
bool pendingHalf = false;
bool pendingResync = true;  // first screen: clean start
bool pendingStress = false;

int cleanEvery() { return CLEAN_CHOICES[cleanChoice]; }

void onButton(Button b, ButtonEvent e, uint32_t heldMs) {
  if (e == ButtonEvent::Pressed) {
    switch (b) {
      case Button::Right:
      case Button::Down: pendingPageDelta++; stressRemaining = 0; break;
      case Button::Left:
      case Button::Up: pendingPageDelta--; stressRemaining = 0; break;
      case Button::Confirm: pendingCycleN = true; break;
      default: break;
    }
  } else if (e == ButtonEvent::LongPress) {
    if (b == Button::Back) pendingResync = true;
    if (b == Button::Power) pendingStress = true;
  } else if (e == ButtonEvent::Released) {
    // A tap = released before the long-press threshold.
    if (b == Button::Back && heldMs < Buttons::LONG_PRESS_MS) pendingHalf = true;
  }
}

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
  canvas.setRotation(3);
  const int W = canvas.width(), H = canvas.height();
  canvas.fillScreen(WHITE);
  canvas.setTextColor(BLACK);

  canvas.setFont(&FreeSansBold18pt7b);
  canvas.setCursor(24, 52);
  canvas.printf("Page %d", page);
  canvas.setFont(&FreeSans9pt7b);
  canvas.setCursor(W - 210, 40);
  if (cleanEvery() > 0)
    canvas.printf("clean every %d (in %d)", cleanEvery(), pagesUntilClean);
  else
    canvas.print("clean-up: off");
  canvas.setCursor(W - 210, 60);
  canvas.printf("FAST since clean: %lu", (unsigned long)fastSinceClean);
  canvas.drawFastHLine(24, 68, W - 48, BLACK);

  canvas.setFont(&FreeSerif12pt7b);
  int y = 105;
  for (int i = 0; i < 3; i++) {
    y = drawWrapped(PARAGRAPHS[(page * 2 + i) % NUM_PARAGRAPHS], 24, y, W - 48, 30) + 10;
  }

  const int spot = page % 4;
  const int sx = (spot % 2) ? W - 24 - 120 : 24;
  const int sy = 470 + (spot / 2) * 140;
  canvas.fillRect(sx, sy, 120, 120, BLACK);

  canvas.drawFastHLine(24, H - 70, W - 48, BLACK);
  canvas.setFont(&FreeSans9pt7b);
  canvas.setCursor(24, H - 46);
  canvas.printf("prev: %s  %lu ms", EpdUc8253::modeName(prev.used), (unsigned long)prev.totalMs);
  canvas.setCursor(24, H - 22);
  canvas.print(stressRemaining > 0 ? "STRESS TEST running..." : "Confirm: N   Back: HALF / hold: RESYNC");
}

void show(Mode mode) {
  drawPage(epd.stats);
  epd.refresh(canvas.getBuffer(), mode);
  const auto& s = epd.stats;
  if (s.used == Mode::Fast) fastSinceClean++;
  else fastSinceClean = 0;
  Serial.printf("[%7lu] page %3d  %-6s total %4lu ms (wave %4lu)%s%s%s\n", (unsigned long)millis(), page,
                EpdUc8253::modeName(s.used), (unsigned long)s.totalMs, (unsigned long)s.waveformMs,
                stressRemaining > 0 ? "  [stress]" : "", s.why[0] ? "  upgraded: " : "", s.why);
  lastActivity = millis();
}

// A normal page turn: FAST, or the periodic clean-up when the counter runs out.
void turnPage() {
  Mode m = Mode::Fast;
  if (cleanEvery() > 0) {
    if (pagesUntilClean <= 1) {
      m = PERIODIC_CLEAN;
      pagesUntilClean = cleanEvery();
    } else {
      pagesUntilClean--;
    }
  }
  show(m);
}

void whileRefreshing() { buttons.update(); }

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  while (!Serial && millis() < 4000) delay(10);
  delay(500);
  Serial.println("\n=== Xteink X3 stage 5: ghost management ===");

  pinMode(pins::SD_POWER, OUTPUT);
  digitalWrite(pins::SD_POWER, HIGH);

  buttons.begin(onButton);
  epd.begin();
  epd.setIdleHook(whileRefreshing);
}

void loop() {
  buttons.update();

  if (pendingResync) {
    pendingResync = false;
    show(Mode::Resync);
  } else if (pendingHalf) {
    pendingHalf = false;
    show(Mode::Half);
  } else if (pendingCycleN) {
    pendingCycleN = false;
    cleanChoice = (cleanChoice + 1) % NUM_CLEAN_CHOICES;
    pagesUntilClean = cleanEvery();
    Serial.printf("clean every -> %d\n", cleanEvery());
    show(Mode::Fast);
  } else if (pendingStress) {
    pendingStress = false;
    stressRemaining = STRESS_TURNS;
    Serial.printf("stress test: %d FAST turns\n", STRESS_TURNS);
  } else if (stressRemaining > 0) {
    page++;
    show(Mode::Fast);  // deliberately no clean-up
    if (--stressRemaining == 0) Serial.println("stress test done");
    delay(300);
  } else if (pendingPageDelta != 0) {
    page += pendingPageDelta;
    if (page < 1) page = 1;
    pendingPageDelta = 0;
    turnPage();
  } else if (epd.powered() && millis() - lastActivity > RAILS_OFF_AFTER_MS) {
    epd.powerOff();
    Serial.printf("[%7lu] idle: panel rails off\n", (unsigned long)millis());
  }
  delay(1);
}
