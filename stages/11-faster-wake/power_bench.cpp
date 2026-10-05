#include "power_bench.h"

#include <driver/gpio.h>
#include <esp_sleep.h>

#include "battery.h"
#include "board_pins.h"

namespace power_bench {

bool lightSleep(uint32_t ms) {
  const auto latch = static_cast<gpio_num_t>(pins::SD_POWER);
  const auto power = static_cast<gpio_num_t>(pins::BTN_POWER);
  gpio_hold_en(latch);  // GPIO13 = battery latch: must not float while asleep
  esp_sleep_enable_timer_wakeup(uint64_t(ms) * 1000);
  gpio_wakeup_enable(power, GPIO_INTR_LOW_LEVEL);  // level-triggered (G17d)
  esp_sleep_enable_gpio_wakeup();
  esp_light_sleep_start();  // returns here on wake; RAM and state intact
  gpio_hold_dis(latch);
  const bool byKey = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO;
  // Wake sources persist: left armed, the 50 ms timer would also end the
  // reader's later DEEP sleep after 50 ms. Disarm everything we armed.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  gpio_wakeup_disable(power);
  return byKey;
}

namespace {
enum class Kind { Busy, Sleep };
struct ModeSpec {
  const char* name;
  Kind kind;
  uint32_t cpuMhz;
  uint32_t sleepMs;
};
const ModeSpec MODES[NUM_MODES] = {
    {"160 MHz delay(1)", Kind::Busy, 160, 0},
    {"80 MHz delay(1)", Kind::Busy, 80, 0},
    {"40 MHz delay(1)", Kind::Busy, 40, 0},
    {"light sleep 50ms", Kind::Sleep, 160, 50},
    {"light sleep 100ms", Kind::Sleep, 160, 100},
    {"LS 50ms + panel on", Kind::Sleep, 160, 50},  // panel rails up, as for 30 s after a turn
};
constexpr uint32_t SETTLE_MS = 4000;   // let the gauge's 1 s average forget the last mode
constexpr uint32_t MEASURE_MS = 16000;

// Spend `ms` in the mode, calling `sample` about once per second.
template <typename F>  // template: works with any callable (Python: duck typing)
void hold(const ModeSpec& m, uint32_t ms, F sample) {
  const uint32_t start = millis();
  uint32_t nextSample = start + 1000;
  while (millis() - start < ms) {
    if (m.kind == Kind::Busy) {
      delay(1);  // what the reader's loop() does today
    } else {
      lightSleep(m.sleepMs);
      analogRead(pins::BTN_ADC_1);  // the work a real wake would do: check the keys
      analogRead(pins::BTN_ADC_2);
    }
    if ((int32_t)(millis() - nextSample) >= 0) {
      sample();
      nextSample += 1000;
    }
  }
}
}  // namespace

void run(Result results[NUM_MODES], void (*panelRails)(bool on)) {
  for (int i = 0; i < NUM_MODES; i++) {
    const ModeSpec& m = MODES[i];
    setCpuFrequencyMhz(m.cpuMhz);
    if (i == NUM_MODES - 1) panelRails(true);
    hold(m, SETTLE_MS, [] {});

    int32_t sum = 0;
    int16_t lo = 32767, hi = -32768;
    uint32_t n = 0;
    hold(m, MEASURE_MS, [&] {  // [&]: the lambda can modify sum/lo/hi/n
      const BatteryReading b = battery::read();
      if (!b.ok) return;
      const int16_t draw = -b.mA;  // discharge as a positive number
      sum += draw;
      lo = min(lo, draw);
      hi = max(hi, draw);
      n++;
    });
    results[i] = {m.name, n ? float(sum) / n : 0.0f, lo, hi, n};
  }
  panelRails(false);
  setCpuFrequencyMhz(160);
}

}  // namespace power_bench
