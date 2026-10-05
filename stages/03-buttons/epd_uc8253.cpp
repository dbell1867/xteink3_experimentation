#include "epd_uc8253.h"

#include <SPI.h>

#include "board_pins.h"
#include "uc8253_x3_luts.h"

namespace {
// UC8253 commands used here
constexpr uint8_t CMD_PANEL_SETTING = 0x00;
constexpr uint8_t CMD_POWER_SETTING = 0x01;
constexpr uint8_t CMD_POWER_OFF = 0x02;
constexpr uint8_t CMD_POWER_OFF_SEQ = 0x03;
constexpr uint8_t CMD_POWER_ON = 0x04;
constexpr uint8_t CMD_BOOSTER_SOFT_START = 0x06;
constexpr uint8_t CMD_DTM1 = 0x10;  // "old" image RAM
constexpr uint8_t CMD_DATA_STOP = 0x11;
constexpr uint8_t CMD_DISPLAY_REFRESH = 0x12;
constexpr uint8_t CMD_DTM2 = 0x13;  // "new" image RAM
constexpr uint8_t CMD_LUT_VCOM = 0x20;
constexpr uint8_t CMD_LUT_WW = 0x21;
constexpr uint8_t CMD_LUT_BW = 0x22;
constexpr uint8_t CMD_LUT_WB = 0x23;
constexpr uint8_t CMD_LUT_BB = 0x24;
constexpr uint8_t CMD_PLL_CONTROL = 0x30;
constexpr uint8_t CMD_VCOM_DATA_INTERVAL = 0x50;  // "CDI"
constexpr uint8_t CMD_RESOLUTION = 0x61;
constexpr uint8_t CMD_GATE_SOURCE_START = 0x65;
constexpr uint8_t CMD_VCOM_DC = 0x82;
constexpr uint8_t CMD_LV_SELECTION = 0xE1;

const SPISettings kSpi(10000000, MSBFIRST, SPI_MODE0);  // 10 MHz, as the X3 board profile
}  // namespace

// ------------------------------------------------------------ low level ---

void EpdUc8253::cmd(uint8_t c) {
  SPI.beginTransaction(kSpi);
  digitalWrite(pins::EPD_DC, LOW);  // LOW = command byte
  digitalWrite(pins::EPD_CS, LOW);
  SPI.transfer(c);
  digitalWrite(pins::EPD_CS, HIGH);
  SPI.endTransaction();
}

void EpdUc8253::data(uint8_t d) {
  SPI.beginTransaction(kSpi);
  digitalWrite(pins::EPD_DC, HIGH);  // HIGH = data byte
  digitalWrite(pins::EPD_CS, LOW);
  SPI.transfer(d);
  digitalWrite(pins::EPD_CS, HIGH);
  SPI.endTransaction();
}

void EpdUc8253::cmdData(uint8_t c, const uint8_t* d, size_t len) {
  cmd(c);
  SPI.beginTransaction(kSpi);
  digitalWrite(pins::EPD_DC, HIGH);
  digitalWrite(pins::EPD_CS, LOW);
  SPI.writeBytes(d, len);
  digitalWrite(pins::EPD_CS, HIGH);
  SPI.endTransaction();
}

// Fill a whole RAM plane with one byte value (0xFF = all white).
void EpdUc8253::fillPlane(uint8_t ramCmd, uint8_t value) {
  uint8_t row[ROW_BYTES];
  memset(row, value, sizeof(row));
  cmd(ramCmd);
  SPI.beginTransaction(kSpi);
  digitalWrite(pins::EPD_DC, HIGH);
  digitalWrite(pins::EPD_CS, LOW);
  for (uint16_t y = 0; y < HEIGHT; y++) SPI.writeBytes(row, ROW_BYTES);
  digitalWrite(pins::EPD_CS, HIGH);
  SPI.endTransaction();
}

// Send the frame bottom row first: the X3 panel is mounted so that the
// controller's first row is the bottom of the screen.
void EpdUc8253::sendPlaneFlipped(uint8_t ramCmd, const uint8_t* fb) {
  cmd(ramCmd);
  SPI.beginTransaction(kSpi);
  digitalWrite(pins::EPD_DC, HIGH);
  digitalWrite(pins::EPD_CS, LOW);
  for (int y = HEIGHT - 1; y >= 0; y--) SPI.writeBytes(fb + y * ROW_BYTES, ROW_BYTES);
  digitalWrite(pins::EPD_CS, HIGH);
  SPI.endTransaction();
}

void EpdUc8253::reset() {
  digitalWrite(pins::EPD_RST, HIGH);
  delay(10);
  digitalWrite(pins::EPD_RST, LOW);
  delay(10);
  digitalWrite(pins::EPD_RST, HIGH);
  delay(10 + 50);  // X3 needs an extra 50 ms settle after reset
}

// X3 BUSY is "two-phase": after a command BUSY drops LOW while the controller
// works, then returns HIGH when done. Wait for both edges and time them.
EpdUc8253::BusyTiming EpdUc8253::waitBusyTwoPhase(uint32_t startTimeoutMs, uint32_t workTimeoutMs) {
  BusyTiming t{0, 0, false};
  const uint32_t t0 = millis();
  while (digitalRead(pins::EPD_BUSY) == HIGH && millis() - t0 < startTimeoutMs) idle();
  t.toLowMs = millis() - t0;
  if (digitalRead(pins::EPD_BUSY) == HIGH) return t;  // never started

  t.sawLow = true;
  const uint32_t t1 = millis();
  while (digitalRead(pins::EPD_BUSY) == LOW && millis() - t1 < workTimeoutMs) idle();
  t.lowMs = millis() - t1;
  return t;
}

// ---------------------------------------------------------------- public ---

void EpdUc8253::begin() {
  // The SD card shares SCLK/MOSI: keep it deselected.
  pinMode(pins::SD_CS, OUTPUT);
  digitalWrite(pins::SD_CS, HIGH);

  pinMode(pins::EPD_CS, OUTPUT);
  digitalWrite(pins::EPD_CS, HIGH);
  pinMode(pins::EPD_DC, OUTPUT);
  digitalWrite(pins::EPD_DC, HIGH);
  pinMode(pins::EPD_RST, OUTPUT);
  pinMode(pins::EPD_BUSY, INPUT);
  SPI.begin(pins::EPD_SCLK, pins::SD_MISO, pins::EPD_MOSI, -1);  // we drive CS ourselves

  reset();

  // Controller setup, byte-for-byte from freeink-sdk initController().
  const uint8_t panel[] = {0x3F, 0x0A};
  cmdData(CMD_PANEL_SETTING, panel, sizeof(panel));
  const uint8_t res[] = {0x03, 0x18, 0x02, 0x58};  // 0x318 = 792, 0x258 = 600 (open question: panel is 528)
  cmdData(CMD_RESOLUTION, res, sizeof(res));
  const uint8_t start[] = {0x00, 0x00, 0x00, 0x00};
  cmdData(CMD_GATE_SOURCE_START, start, sizeof(start));
  cmd(CMD_POWER_OFF_SEQ);
  data(0x20);
  const uint8_t power[] = {0x07, 0x17, 0x3F, 0x3F, 0x17};
  cmdData(CMD_POWER_SETTING, power, sizeof(power));
  cmd(CMD_VCOM_DC);
  data(0x24);
  const uint8_t booster[] = {0x25, 0x25, 0x3C, 0x37};
  cmdData(CMD_BOOSTER_SOFT_START, booster, sizeof(booster));
  cmd(CMD_PLL_CONTROL);
  data(0x09);
  cmd(CMD_LV_SELECTION);
  data(0x02);

  // UC8253 doesn't clear its RAM on reset: make both planes white.
  fillPlane(CMD_DTM1, 0xFF);
  cmd(CMD_DATA_STOP);
  fillPlane(CMD_DTM2, 0xFF);
  cmd(CMD_DATA_STOP);
}

void EpdUc8253::displayFull(const uint8_t* fb) {
  const uint32_t t0 = millis();

  // 1. Select the OEM FULL waveform: CDI then the five LUTs.
  const uint8_t cdi[] = {0x29, 0x07};
  cmdData(CMD_VCOM_DATA_INTERVAL, cdi, sizeof(cdi));
  cmdData(CMD_LUT_VCOM, uc8253::LUT_FULL_VCOM, sizeof(uc8253::LUT_FULL_VCOM));
  cmdData(CMD_LUT_WW, uc8253::LUT_FULL_WW, sizeof(uc8253::LUT_FULL_WW));
  cmdData(CMD_LUT_BW, uc8253::LUT_FULL_BW, sizeof(uc8253::LUT_FULL_BW));
  cmdData(CMD_LUT_WB, uc8253::LUT_FULL_WB, sizeof(uc8253::LUT_FULL_WB));
  cmdData(CMD_LUT_BB, uc8253::LUT_FULL_BB, sizeof(uc8253::LUT_FULL_BB));

  // 2. Old image = white, new image = our frame.
  fillPlane(CMD_DTM1, 0xFF);
  cmd(CMD_DATA_STOP);
  sendPlaneFlipped(CMD_DTM2, fb);
  sendMs = millis() - t0;

  // 3. Power the panel's high-voltage rails, refresh, power off.
  cmd(CMD_POWER_ON);
  tPowerOn = waitBusyTwoPhase();
  cmd(CMD_DISPLAY_REFRESH);
  tRefresh = waitBusyTwoPhase();
  cmd(CMD_POWER_OFF);
  tPowerOff = waitBusyTwoPhase();
}
