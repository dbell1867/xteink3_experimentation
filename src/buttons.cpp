#include "buttons.h"

#include "board_pins.h"

namespace {
// Above this, nothing is pressed (idle reads 4095, Back ~3480).
constexpr int ADC_NO_BUTTON = 3900;

// Ladder 1 (GPIO1): Back ~3483, Confirm ~2663, Left ~1469, Right ~0.
// Thresholds are midpoints between neighbours (same as freeink-sdk).
Button classifyLadder1(int raw) {
  if (raw > ADC_NO_BUTTON) return Button::None;
  if (raw > 3100) return Button::Back;
  if (raw > 2090) return Button::Confirm;
  if (raw > 750) return Button::Left;
  return Button::Right;
}

// Ladder 2 (GPIO2): Up ~2207, Down ~4.
Button classifyLadder2(int raw) {
  if (raw > ADC_NO_BUTTON) return Button::None;
  if (raw > 1120) return Button::Up;
  return Button::Down;
}
}  // namespace

const char* buttonName(Button b) {
  switch (b) {
    case Button::Back: return "Back";
    case Button::Confirm: return "Confirm";
    case Button::Left: return "Left";
    case Button::Right: return "Right";
    case Button::Up: return "Up";
    case Button::Down: return "Down";
    case Button::Power: return "Power";
    default: return "None";
  }
}

void Buttons::begin(Handler handler) {
  onEvent = handler;
  analogSetAttenuation(ADC_11db);
  pinMode(pins::BTN_ADC_1, INPUT);
  pinMode(pins::BTN_ADC_2, INPUT);
  pinMode(pins::BTN_POWER, INPUT_PULLUP);
}

void Buttons::update() {
  const uint32_t now = millis();
  if (now - lastSample < SAMPLE_MS) return;
  lastSample = now;

  const int a1 = analogRead(pins::BTN_ADC_1);
  const int a2 = analogRead(pins::BTN_ADC_2);
  const bool pwr = digitalRead(pins::BTN_POWER) == LOW;

  process(ch[0], classifyLadder1(a1), a1, now, "ladder1");
  process(ch[1], classifyLadder2(a2), a2, now, "ladder2");
  process(ch[2], pwr ? Button::Power : Button::None, pwr ? 0 : 1, now, "power");
}

bool Buttons::busy() const {
  for (const Channel& c : ch)
    if (c.stable != Button::None || c.candidate != Button::None) return true;
  return false;
}

void Buttons::process(Channel& c, Button raw, int rawValue, uint32_t now, const char* label) {
  // A new raw reading restarts the stability timer. If the previous candidate
  // differed from the accepted state and never lasted STABLE_MS, it was a glitch.
  if (raw != c.candidate) {
    if (c.candidate != c.stable) {
      glitches++;
      Serial.printf("[%7lu] glitch %s: saw %s (raw %d) for %lu ms, now %s (raw %d)\n", (unsigned long)now, label,
                    buttonName(c.candidate), c.lastRaw, (unsigned long)(now - c.candidateSince), buttonName(raw),
                    rawValue);
    }
    c.candidate = raw;
    c.candidateSince = now;
  }
  c.lastRaw = rawValue;

  // Accept the candidate once it has been stable long enough.
  if (c.candidate != c.stable && now - c.candidateSince >= STABLE_MS) {
    if (c.stable != Button::None && onEvent) onEvent(c.stable, ButtonEvent::Released, now - c.pressedAt);
    c.stable = c.candidate;
    if (c.stable != Button::None) {
      c.pressedAt = now;
      c.longSent = false;
      if (onEvent) onEvent(c.stable, ButtonEvent::Pressed, 0);
    }
  }

  // Long press fires once while still held.
  if (c.stable != Button::None && !c.longSent && now - c.pressedAt >= LONG_PRESS_MS) {
    c.longSent = true;
    if (onEvent) onEvent(c.stable, ButtonEvent::LongPress, now - c.pressedAt);
  }
}
