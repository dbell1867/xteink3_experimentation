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
constexpr uint8_t CMD_DEEP_SLEEP = 0x07;
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
constexpr uint8_t CMD_PARTIAL_WINDOW = 0x90;
constexpr uint8_t CMD_PARTIAL_IN = 0x91;
constexpr uint8_t CMD_PARTIAL_OUT = 0x92;

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
// works, then returns HIGH when done. Wait for both edges; return the LOW time
// (0 if BUSY never dropped, e.g. power-on when the rails were already up).
uint32_t EpdUc8253::waitBusyTwoPhase(uint32_t startTimeoutMs, uint32_t workTimeoutMs) {
  const uint32_t t0 = millis();
  while (digitalRead(pins::EPD_BUSY) == HIGH && millis() - t0 < startTimeoutMs) idle();
  if (digitalRead(pins::EPD_BUSY) == HIGH) return 0;  // never started

  const uint32_t t1 = millis();
  while (digitalRead(pins::EPD_BUSY) == LOW && millis() - t1 < workTimeoutMs) idle();
  return millis() - t1;
}

// CDI (VCOM and data interval) then the five transition LUTs.
void EpdUc8253::loadBank(uint8_t cdi0, const LutBank& bank) {
  const uint8_t cdi[] = {cdi0, 0x07};
  cmdData(CMD_VCOM_DATA_INTERVAL, cdi, sizeof(cdi));
  cmdData(CMD_LUT_VCOM, bank.vcom, 42);
  cmdData(CMD_LUT_WW, bank.ww, 42);
  cmdData(CMD_LUT_BW, bank.bw, 42);
  cmdData(CMD_LUT_WB, bank.wb, 42);
  cmdData(CMD_LUT_BB, bank.bb, 42);
}

// Partial-window mode covering the whole panel (how the SDK runs NORMAL passes).
void EpdUc8253::fullScreenWindowIn() {
  const uint16_t xEnd = WIDTH - 1, yEnd = HEIGHT - 1;
  const uint8_t w[9] = {0, 0, uint8_t(xEnd >> 8), uint8_t(xEnd & 0xFF), 0, 0, uint8_t(yEnd >> 8), uint8_t(yEnd & 0xFF),
                        0x01};
  cmd(CMD_PARTIAL_IN);
  cmdData(CMD_PARTIAL_WINDOW, w, sizeof(w));
}

// Raise the panel's high-voltage rails once; a redundant POWER_ON gives no
// BUSY edge and would burn the 1 s start timeout.
uint32_t EpdUc8253::powerOnIfNeeded() {
  if (isPowered) return 0;
  cmd(CMD_POWER_ON);
  const uint32_t ms = waitBusyTwoPhase();
  isPowered = true;
  return ms;
}

uint32_t EpdUc8253::runWaveform() {
  cmd(CMD_DISPLAY_REFRESH);
  return waitBusyTwoPhase();
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
  isPowered = false;
  oldPlaneValid = false;  // RAM is white but the glass shows whatever it had
  firstFullDone = false;
}

const char* EpdUc8253::modeName(Mode m) {
  switch (m) {
    case Mode::Resync: return "RESYNC";
    case Mode::Full: return "FULL";
    case Mode::Half: return "HALF";
    case Mode::Normal: return "NORMAL";
    case Mode::Fast: return "FAST";
  }
  return "?";
}

void EpdUc8253::refresh(const uint8_t* fb, Mode mode) {
  using namespace uc8253;  // lets us write LUT_FULL_WW instead of uc8253::LUT_FULL_WW
  static const LutBank FULL = {LUT_FULL_VCOM, LUT_FULL_WW, LUT_FULL_BW, LUT_FULL_WB, LUT_FULL_BB};
  static const LutBank HALF = {LUT_HALF_VCOM, LUT_HALF_WW, LUT_HALF_BW, LUT_HALF_WB, LUT_HALF_BB};
  static const LutBank NORMAL = {LUT_NORMAL_VCOM, LUT_NORMAL_WW, LUT_NORMAL_BW, LUT_NORMAL_WB, LUT_NORMAL_BB};
  static const LutBank FAST = {LUT_FAST_VCOM, LUT_FAST_WW, LUT_FAST_BW, LUT_FAST_WB, LUT_FAST_BB};

  stats = Stats{};
  stats.requested = mode;
  const uint32_t t0 = millis();

  // RESYNC = FULL that always gets the conditioning pass (see below).
  const bool wantResync = (mode == Mode::Resync);
  if (wantResync) mode = Mode::Full;

  // Upgrade rules (from freeink-sdk):
  //  - no trustworthy old frame in DTM1 -> only FULL can draw correctly
  //  - rails were off -> differential modes get the stronger HALF waveform
  if (mode != Mode::Full && !oldPlaneValid) {
    mode = Mode::Full;
    stats.why = "no old frame";
  } else if ((mode == Mode::Fast || mode == Mode::Normal) && !isPowered) {
    mode = Mode::Half;
    stats.why = "rails were off";
  }
  // Decide after the upgrades: the first FULL after begin() is always conditioned.
  const bool conditioning = wantResync || (mode == Mode::Full && !firstFullDone);
  stats.used = conditioning ? Mode::Resync : mode;

  // 1. Waveform + image upload
  switch (mode) {
    case Mode::Full:
      loadBank(0x29, FULL);
      fillPlane(CMD_DTM1, 0xFF);  // old = white baseline
      cmd(CMD_DATA_STOP);
      sendPlaneFlipped(CMD_DTM2, fb);
      break;
    case Mode::Half:
      loadBank(0xA9, HALF);
      sendPlaneFlipped(CMD_DTM2, fb);
      break;
    case Mode::Normal:
      loadBank(0xA9, NORMAL);
      fullScreenWindowIn();
      sendPlaneFlipped(CMD_DTM2, fb);
      cmd(CMD_PARTIAL_OUT);
      break;
    case Mode::Fast:
      loadBank(0x29, FAST);
      sendPlaneFlipped(CMD_DTM2, fb);
      break;
  }
  stats.uploadMs = millis() - t0;

  // 2. The visible part
  stats.powerOnMs = powerOnIfNeeded();
  stats.waveformMs = runWaveform();
  const uint32_t t1 = millis();

  // 3. Housekeeping so the NEXT differential refresh is clean
  if (mode != Mode::Fast) delay(200);  // SDK settle after non-turbo waveforms

  if (conditioning) {
    // RESYNC, or the first FULL after begin(): one OEM "normal" conditioning pass.
    loadBank(0xA9, NORMAL);
    fullScreenWindowIn();
    sendPlaneFlipped(CMD_DTM2, fb);
    cmd(CMD_PARTIAL_OUT);
    runWaveform();
    firstFullDone = true;
  }

  sendPlaneFlipped(CMD_DTM1, fb);  // old := what's on the glass now
  cmd(CMD_DATA_STOP);
  oldPlaneValid = true;

  if (mode == Mode::Full) {
    // SDK: "first differential after a full garbles on X3" -> spend a no-op
    // FAST pass of the same frame so the caller's next diff is clean.
    loadBank(0x29, FAST);
    sendPlaneFlipped(CMD_DTM2, fb);
    runWaveform();
    sendPlaneFlipped(CMD_DTM1, fb);
    cmd(CMD_DATA_STOP);
  }

  stats.extraMs = millis() - t1;
  stats.totalMs = millis() - t0;
}

void EpdUc8253::powerOff() {
  if (!isPowered) return;
  cmd(CMD_POWER_OFF);
  waitBusyTwoPhase();
  isPowered = false;
}

void EpdUc8253::deepSleep() {
  powerOff();
  cmd(CMD_DEEP_SLEEP);
  data(0xA5);  // check code required by the controller
  oldPlaneValid = false;
}
