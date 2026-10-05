// TI BQ27220 battery fuel gauge on the X3's I2C bus (SDA 20, SCL 0, addr 0x55).
// Read-only: we never write the gauge's configuration.
#pragma once
#include <Arduino.h>

struct BatteryReading {
  bool ok = false;      // false if the gauge didn't answer
  uint16_t percent = 0; // StateOfCharge()
  uint16_t mV = 0;      // Voltage()
  int16_t mA = 0;       // Current(): + = charging, - = discharging (1 s average)
};

namespace battery {
void begin();
BatteryReading read();
}  // namespace battery
