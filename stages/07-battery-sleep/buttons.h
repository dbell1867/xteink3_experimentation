// Xteink X3 buttons: two ADC resistor ladders + a digital power key.
// Thresholds from measured values (docs/data/buttons-x3.csv).
#pragma once
#include <Arduino.h>

// `enum class` = a named set of constants that don't leak into the global
// namespace (Python: like an Enum; you write Button::Back, not just Back).
enum class Button : uint8_t { None, Back, Confirm, Left, Right, Up, Down, Power };
const char* buttonName(Button b);

enum class ButtonEvent : uint8_t { Pressed, LongPress, Released };

class Buttons {
 public:
  static constexpr uint32_t SAMPLE_MS = 5;       // how often update() really samples
  static constexpr uint32_t STABLE_MS = 20;      // reading must hold this long to count
  static constexpr uint32_t LONG_PRESS_MS = 700; // same as freeink-sdk's home-key hold

  // The callback gets: which button, what happened, and how long it has been held.
  using Handler = void (*)(Button, ButtonEvent, uint32_t heldMs);

  void begin(Handler handler);
  void update();  // call as often as you like; samples every SAMPLE_MS

  uint32_t glitches = 0;  // readings that changed but didn't last STABLE_MS

 private:
  // One input line (ladder 1, ladder 2 or power), debounced on its own.
  struct Channel {
    Button stable = Button::None;     // accepted state
    Button candidate = Button::None;  // latest raw reading
    uint32_t candidateSince = 0;
    uint32_t pressedAt = 0;
    bool longSent = false;
    int lastRaw = 0;
  };
  Channel ch[3];
  Handler onEvent = nullptr;
  uint32_t lastSample = 0;

  void process(Channel& c, Button raw, int rawValue, uint32_t now, const char* label);
};
