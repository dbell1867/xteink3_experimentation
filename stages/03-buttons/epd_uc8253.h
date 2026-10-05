// Minimal UC8253 driver for the Xteink X3 panel (792x528, 1 bit per pixel).
// Follows freeink-sdk's Uc8253X3Driver FULL-refresh path; see docs/hardware-x3.md.
#pragma once
#include <Arduino.h>

class EpdUc8253 {
 public:
  static constexpr uint16_t WIDTH = 792;
  static constexpr uint16_t HEIGHT = 528;
  static constexpr uint16_t ROW_BYTES = WIDTH / 8;  // 99 bytes per row

  // Timing of one BUSY wait: how long until BUSY went LOW (started),
  // and how long it then stayed LOW (working). sawLow=false -> never started.
  struct BusyTiming {
    uint32_t toLowMs;
    uint32_t lowMs;
    bool sawLow;
  };

  void begin();  // SPI + reset + controller init (both RAM planes white)

  // Show a full frame with one FULL (flashing, cleanest) refresh.
  // fb: HEIGHT rows of ROW_BYTES, MSB = leftmost pixel, bit 1 = white.
  void displayFull(const uint8_t* fb);

  // Called every ~1 ms while waiting on BUSY, so other work (e.g. reading
  // buttons) keeps running during the ~1.2 s refresh. nullptr = none.
  void setIdleHook(void (*hook)()) { idleHook = hook; }

  // Results of the last displayFull(), for printing.
  BusyTiming tPowerOn{}, tRefresh{}, tPowerOff{};
  uint32_t sendMs = 0;

 private:
  void (*idleHook)() = nullptr;  // pointer to a function (Python: a callback)
  void idle() {
    if (idleHook) idleHook();
    delay(1);
  }
  void cmd(uint8_t c);
  void data(uint8_t d);
  void cmdData(uint8_t c, const uint8_t* d, size_t len);
  void fillPlane(uint8_t ramCmd, uint8_t value);
  void sendPlaneFlipped(uint8_t ramCmd, const uint8_t* fb);
  void reset();
  BusyTiming waitBusyTwoPhase(uint32_t startTimeoutMs = 1000, uint32_t workTimeoutMs = 30000);
};
