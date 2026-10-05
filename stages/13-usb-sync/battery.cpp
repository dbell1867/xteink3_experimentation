#include "battery.h"

#include <Wire.h>

#include "board_pins.h"

namespace {
constexpr uint8_t REG_VOLTAGE = 0x08;
constexpr uint8_t REG_CURRENT = 0x0C;
constexpr uint8_t REG_STATE_OF_CHARGE = 0x2C;

// BQ27220 "standard commands" are 16-bit little-endian registers.
bool readU16(uint8_t reg, uint16_t& out) {
  Wire.beginTransmission(i2caddr::BQ27220);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(i2caddr::BQ27220, uint8_t(2)) != 2) return false;
  out = Wire.read() | (Wire.read() << 8);
  return true;
}
}  // namespace

namespace battery {

void begin() { Wire.begin(pins::I2C_SDA, pins::I2C_SCL, 400000); }

BatteryReading read() {
  BatteryReading r;
  uint16_t soc, mv, ma;
  r.ok = readU16(REG_STATE_OF_CHARGE, soc) && readU16(REG_VOLTAGE, mv) && readU16(REG_CURRENT, ma);
  if (r.ok) {
    r.percent = soc;
    r.mV = mv;
    r.mA = static_cast<int16_t>(ma);  // reinterpret the 16 bits as signed
  }
  return r;
}

}  // namespace battery
