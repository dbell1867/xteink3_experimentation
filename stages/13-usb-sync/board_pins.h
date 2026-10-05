// Xteink X3 pin map. Source: freeink-sdk BoardConfig.h (XTEINK_X3 profile),
// see docs/hardware-x3.md. Values marked "todo" there are verified by stage 1.
#pragma once
#include <cstdint>

namespace pins {

// E-paper display (SPI, 792x528). MOSI doubles as the half-duplex read line.
constexpr int EPD_SCLK = 8;
constexpr int EPD_MOSI = 10;
constexpr int EPD_CS   = 21;
constexpr int EPD_DC   = 4;
constexpr int EPD_RST  = 5;
constexpr int EPD_BUSY = 6;

// microSD (shares SCLK/MOSI with the display)
constexpr int SD_MISO  = 7;
constexpr int SD_CS    = 12;
constexpr int SD_POWER = 13;  // HIGH = card powered

// I2C bus: fuel gauge, RTC, IMU
constexpr int I2C_SDA = 20;
constexpr int I2C_SCL = 0;

// Buttons: two resistor ladders on ADC pins + a digital power key
constexpr int BTN_ADC_1 = 1;  // Back / Confirm / Left / Right
constexpr int BTN_ADC_2 = 2;  // Up / Down
constexpr int BTN_POWER = 3;  // active LOW

}  // namespace pins

namespace i2caddr {
constexpr uint8_t BQ27220 = 0x55;  // battery fuel gauge
constexpr uint8_t DS3231  = 0x68;  // real-time clock
constexpr uint8_t QMI8658 = 0x6B;  // IMU (alt 0x6A)
}  // namespace i2caddr
