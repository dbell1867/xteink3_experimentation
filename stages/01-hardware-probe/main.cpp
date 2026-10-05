// Stage 1 — "hello + hardware probe" for the Xteink X3.
//
// Read-only: nothing is drawn on the screen and nothing is written to the
// I2C chips. Each probe prints what it finds so we can compare against the
// predictions in docs/hardware-x3.md.
//
//   1. chip info        -> proves build / flash / serial works
//   2. I2C scan         -> expect 0x55 (gauge), 0x68 (RTC), 0x6B (IMU)
//   3. display probe    -> UC8279d answers cmd 0x70/0x71; UC8253 stays silent
//   4. button monitor   -> raw ADC values while you press each key (runs forever)

#include <Arduino.h>
#include <Wire.h>

#include "board_pins.h"

// ---------------------------------------------------------------- helpers ---

// Read `len` bytes starting at register `reg` of I2C device `addr`.
// Returns false if the device didn't answer.
bool i2cReadRegs(uint8_t addr, uint8_t reg, uint8_t* out, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;  // false = keep bus (repeated start)
  if (Wire.requestFrom(addr, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) out[i] = Wire.read();
  return true;
}

uint8_t bcdToDec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

// ------------------------------------------------------------ 1. chip info ---

void printChipInfo() {
  Serial.println();
  Serial.println("=== Xteink X3 hardware probe (stage 1) ===");
  Serial.printf("Chip: %s rev %d, %d core(s), %lu MHz\n", ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getChipCores(), (unsigned long)ESP.getCpuFreqMHz());
  Serial.printf("Flash: %lu MB, free heap: %lu bytes, PSRAM: %lu bytes\n",
                (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)), (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getPsramSize());
}

// ------------------------------------------------------------- 2. I2C scan ---

void scanI2c() {
  Serial.println("\n--- I2C scan (SDA=20, SCL=0, 100 kHz) ---");
  Wire.begin(pins::I2C_SDA, pins::I2C_SCL, 100000);

  // Scan every legal 7-bit address, not just the three we expect: a probe
  // that only looks where we predict can't surprise us.
  int found = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  found 0x%02X\n", addr);
      found++;
    }
  }
  Serial.printf("  %d device(s)\n", found);

  uint8_t buf[3];

  // QMI8658 IMU: WHO_AM_I (reg 0x00) should read 0x05. Try the alternate
  // address only if the main one is silent (probing an empty address logs an error).
  for (uint8_t addr : {i2caddr::QMI8658, uint8_t(0x6A)}) {
    if (i2cReadRegs(addr, 0x00, buf, 1)) {
      Serial.printf("  QMI8658 @0x%02X WHO_AM_I = 0x%02X (expect 0x05)\n", addr, buf[0]);
      break;
    }
  }

  // BQ27220 fuel gauge: 16-bit little-endian values
  if (i2cReadRegs(i2caddr::BQ27220, 0x08, buf, 2)) {
    uint16_t mv = buf[0] | (buf[1] << 8);
    i2cReadRegs(i2caddr::BQ27220, 0x0C, buf, 2);
    int16_t ma = int16_t(buf[0] | (buf[1] << 8));
    i2cReadRegs(i2caddr::BQ27220, 0x2C, buf, 2);
    uint16_t soc = buf[0] | (buf[1] << 8);
    Serial.printf("  BQ27220 battery: %u mV, %d mA (+ = charging), %u %%\n", mv, ma, soc);
  } else {
    Serial.println("  BQ27220 (0x55) did not answer");
  }

  // DS3231 RTC: seconds/minutes/hours in BCD at regs 0..2
  if (i2cReadRegs(i2caddr::DS3231, 0x00, buf, 3)) {
    Serial.printf("  DS3231 time: %02u:%02u:%02u\n", bcdToDec(buf[2] & 0x3F), bcdToDec(buf[1]),
                  bcdToDec(buf[0] & 0x7F));
  } else {
    Serial.println("  DS3231 (0x68) did not answer");
  }
}

// ------------------------------------------------------- 3. display probe ---
// Bit-banged half-duplex SPI, following freeink-sdk XteinkDetect.cpp.
// We send a command byte, then release MOSI (with a pull-up) and clock bytes
// back in. A UC81xx controller drives real data; a controller without these
// read commands leaves the line floating -> all 0xFF or all 0x00.

void epdWriteByte(uint8_t b) {
  for (int i = 0; i < 8; i++) {
    digitalWrite(pins::EPD_MOSI, (b & 0x80) ? HIGH : LOW);
    delayMicroseconds(1);
    digitalWrite(pins::EPD_SCLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(pins::EPD_SCLK, LOW);
    b <<= 1;
  }
}

uint8_t epdReadByte() {
  uint8_t b = 0;
  for (int i = 0; i < 8; i++) {
    delayMicroseconds(1);
    b = (b << 1) | (digitalRead(pins::EPD_MOSI) == HIGH ? 1 : 0);
    digitalWrite(pins::EPD_SCLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(pins::EPD_SCLK, LOW);
  }
  return b;
}

void epdCmdRead(uint8_t cmd, uint8_t* out, int len) {
  pinMode(pins::EPD_MOSI, OUTPUT);
  digitalWrite(pins::EPD_DC, LOW);  // DC low = command
  digitalWrite(pins::EPD_CS, LOW);
  delayMicroseconds(1);
  epdWriteByte(cmd);
  digitalWrite(pins::EPD_DC, HIGH);  // DC high = data
  pinMode(pins::EPD_MOSI, INPUT_PULLUP);  // let the controller drive the line
  delayMicroseconds(1);
  for (int i = 0; i < len; i++) out[i] = epdReadByte();
  digitalWrite(pins::EPD_CS, HIGH);
  pinMode(pins::EPD_MOSI, OUTPUT);
}

// One probe pass. Returns true if the reply looks like a real UC81xx.
bool epdProbePass(uint32_t rstLowMs, uint8_t ver[5], uint8_t& flg) {
  pinMode(pins::EPD_CS, OUTPUT);
  digitalWrite(pins::EPD_CS, HIGH);
  pinMode(pins::EPD_SCLK, OUTPUT);
  digitalWrite(pins::EPD_SCLK, LOW);
  pinMode(pins::EPD_DC, OUTPUT);
  pinMode(pins::EPD_BUSY, INPUT);

  pinMode(pins::EPD_RST, OUTPUT);  // reset pulse
  digitalWrite(pins::EPD_RST, HIGH);
  delay(2);
  digitalWrite(pins::EPD_RST, LOW);
  delay(rstLowMs);
  digitalWrite(pins::EPD_RST, HIGH);
  delay(30);

  epdCmdRead(0x71, &flg, 1);  // FLG: status, bit0 = 1 when idle
  epdCmdRead(0x70, ver, 5);   // VER: 0x00, CHIP_VER, LUT_VER[3]

  bool floating = true;
  for (int i = 1; i < 5; i++)
    if (ver[i] != ver[0]) floating = false;
  return flg != 0x00 && flg != 0xFF && (flg & 0x01) && !floating;
}

void probeDisplay() {
  Serial.println("\n--- Display controller probe (cmd 0x71 FLG, 0x70 VER) ---");
  uint8_t ver[5], flg;
  bool results[2];
  const uint32_t rstMs[2] = {1, 50};
  for (int pass = 0; pass < 2; pass++) {
    results[pass] = epdProbePass(rstMs[pass], ver, flg);
    Serial.printf("  pass %d (reset %lums): FLG=0x%02X VER=%02X %02X %02X %02X %02X BUSY=%d -> %s\n", pass + 1,
                  (unsigned long)rstMs[pass], flg, ver[0], ver[1], ver[2], ver[3], ver[4],
                  digitalRead(pins::EPD_BUSY), results[pass] ? "UC81xx reply" : "no reply");
  }
  if (results[0] && results[1])
    Serial.println("  verdict: UC8279d (newer X3 run)");
  else if (!results[0] && !results[1])
    Serial.println("  verdict: no UC81xx reply -> UC8253 (original X3 run)");
  else
    Serial.println("  verdict: passes disagree -> inconclusive");

  // Release everything; CS pulled up so the panel stays deselected.
  pinMode(pins::EPD_SCLK, INPUT);
  pinMode(pins::EPD_MOSI, INPUT);
  pinMode(pins::EPD_CS, INPUT_PULLUP);
  pinMode(pins::EPD_DC, INPUT);
  pinMode(pins::EPD_RST, INPUT);
}

// ------------------------------------------------------ 4. button monitor ---
// The X4 thresholds, used only to put a guessed name next to the raw value.
const char* guessLadder1(int raw) {
  if (raw > 3100) return "Back?";
  if (raw > 2090) return "Confirm?";
  if (raw > 750) return "Left?";
  return "Right?/idle?";
}
const char* guessLadder2(int raw) { return raw > 1120 ? "Up?" : "Down?/idle?"; }

int lastA1 = -1000, lastA2 = -1000, lastPwr = -1;
uint32_t lastPrint = 0;

void buttonMonitor() {
  int a1 = analogRead(pins::BTN_ADC_1);
  int a2 = analogRead(pins::BTN_ADC_2);
  int pwr = digitalRead(pins::BTN_POWER);

  bool changed = abs(a1 - lastA1) > 150 || abs(a2 - lastA2) > 150 || pwr != lastPwr;
  if (changed || millis() - lastPrint > 5000) {
    Serial.printf("%s GPIO1=%4d (%-12s) GPIO2=%4d (%-11s) POWER(GPIO3)=%s\n", changed ? "*" : " ", a1,
                  guessLadder1(a1), a2, guessLadder2(a2), pwr == LOW ? "PRESSED" : "up");
    lastA1 = a1;
    lastA2 = a2;
    lastPwr = pwr;
    lastPrint = millis();
  }
}

// ------------------------------------------------------------ setup/loop ---

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // don't block if no one is listening (G11)
  while (!Serial && millis() < 4000) delay(10);  // give the USB host a moment
  delay(500);

  // Keep the SD card powered (as the stock firmware does) but deselected,
  // so it stays quiet while we wiggle the shared SPI lines.
  pinMode(pins::SD_POWER, OUTPUT);
  digitalWrite(pins::SD_POWER, HIGH);
  pinMode(pins::SD_CS, OUTPUT);
  digitalWrite(pins::SD_CS, HIGH);

  printChipInfo();
  scanI2c();
  probeDisplay();

  analogSetAttenuation(ADC_11db);  // full 0..~3.1 V range
  pinMode(pins::BTN_ADC_1, INPUT);
  pinMode(pins::BTN_ADC_2, INPUT);
  pinMode(pins::BTN_POWER, INPUT_PULLUP);
  Serial.println("\n--- Button monitor: press each key one at a time ('*' = changed) ---");
}

void loop() {
  buttonMonitor();
  delay(50);
}
