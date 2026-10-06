// Power bench (skill G20): hold one power mode at a time and measure the
// battery current with the X3's own BQ27220 gauge. Run on battery.
#pragma once
#include <Arduino.h>

namespace power_bench {

struct Result {
  const char* name;
  float avgMa;   // mean of the gauge's 1 s readings (discharge shown as positive)
  int16_t minMa, maxMa;
  uint32_t samples;
  // light-sleep modes only: did the chip actually sleep?
  uint32_t sleeps = 0, rejected = 0;   // esp_light_sleep_start() calls / non-OK returns
  uint32_t avgSleptUs = 0;             // mean real time per sleep (esp_timer)
  uint32_t byTimer = 0, byGpio = 0, byOther = 0;  // wake causes
};

constexpr int NUM_MODES = 6;

// Runs all modes (~2.3 min), restores 160 MHz, fills results[]. Blocking.
// panelRails(on) is called around the last mode (panel high-voltage rails on).
void run(Result results[NUM_MODES], void (*panelRails)(bool on));

// Light sleep for up to `ms`, woken early by the Power key. Holds the battery
// latch (GPIO13) through the sleep. Returns true if the Power key woke it.
bool lightSleep(uint32_t ms);

// Counters filled by lightSleep() (reset by run() per mode).
struct SleepStats {
  uint32_t sleeps, rejected, byTimer, byGpio, byOther;
  uint64_t sleptUs;
  int lastErr;
};
extern SleepStats stats;

}  // namespace power_bench
