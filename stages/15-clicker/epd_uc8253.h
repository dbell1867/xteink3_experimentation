// Minimal UC8253 driver for the Xteink X3 panel (792x528, 1 bit per pixel).
// Follows freeink-sdk's Uc8253X3Driver; see docs/hardware-x3.md.
//
// Refresh modes (fastest last):
//   Resync - Full + one OEM 'normal' conditioning pass: what CrossPoint uses on
//            the X3 for its periodic clean-up (requestResync(1)), ~2.4 s
//   Full   - OEM full waveform from a white baseline: flashes, cleanest, ~1.2 s
//   Half   - "scrub": drives every pixel to its target, ignores the old frame
//   Normal - EXPERIMENTAL: OEM 'normal' LUTs driven like the SDK's conditioning pass;
//            measured 744 ms and looked messier than Fast (stage 4) -> not used for turns
//   Fast   - community "turbo" differential page-turn: 467 ms, the page-turn mode
// Differential modes need the controller's "old" plane (DTM1) to hold what is
// on the glass; the driver tracks that and upgrades the mode when it can't.
#pragma once
#include <Arduino.h>

class EpdUc8253 {
 public:
  static constexpr uint16_t WIDTH = 792;
  static constexpr uint16_t HEIGHT = 528;
  static constexpr uint16_t ROW_BYTES = WIDTH / 8;  // 99 bytes per row

  enum class Mode : uint8_t { Resync, Full, Half, Normal, Fast };
  static const char* modeName(Mode m);

  // What the last refresh() did and how long each part took.
  struct Stats {
    Mode requested = Mode::Full;
    Mode used = Mode::Full;   // differs from requested if the driver upgraded it
    const char* why = "";     // reason for an upgrade
    uint32_t uploadMs = 0;    // LUTs + image over SPI
    uint32_t powerOnMs = 0;   // 0 if the rails were already on
    uint32_t waveformMs = 0;  // the visible refresh (BUSY low); an upper bound if the caller finished late
    uint32_t extraMs = 0;     // settle delay, conditioning passes, DTM1 sync
    uint32_t totalMs = 0;
  };

  void begin();  // SPI + reset + controller init (both RAM planes white)

  // Show a frame. fb: HEIGHT rows of ROW_BYTES, MSB = leftmost pixel, 1 = white.
  // Leaves the panel's high-voltage rails ON; call powerOff() when idle.
  void refresh(const uint8_t* fb, Mode mode);  // = refreshStart() + refreshFinish()

  // Split refresh (as freeink-sdk's displayStart/displayFinish): start returns
  // once the waveform is running (BUSY went LOW), so the caller can do other
  // work during the ~0.5 s the panel draws on its own -- including using the
  // SPI bus for the SD card (separate chip-select). Until refreshFinish(),
  // don't touch `fb` (finish syncs it into the old-frame RAM) or this driver.
  void refreshStart(const uint8_t* fb, Mode mode);
  void refreshFinish();
  void powerOff();
  void powerOn() { powerOnIfNeeded(); }  // raise the panel's high-voltage rails
  // Rails off + controller deep sleep (cmd 0x07, 0xA5). The image stays on the
  // glass. Needs begin() again (hardware reset) before the next refresh.
  void deepSleep();
  bool powered() const { return isPowered; }

  // Called every ~1 ms while waiting on BUSY, so other work (e.g. reading
  // buttons) keeps running during a refresh. nullptr = none.
  void setIdleHook(void (*hook)()) { idleHook = hook; }

  Stats stats;

 private:
  struct LutBank {
    const uint8_t* vcom;
    const uint8_t* ww;
    const uint8_t* bw;
    const uint8_t* wb;
    const uint8_t* bb;
  };

  // State carried from refreshStart() to refreshFinish()
  struct Pending {
    bool active = false;
    const uint8_t* fb = nullptr;
    Mode mode = Mode::Full;
    bool conditioning = false;
    uint32_t t0 = 0, tWave = 0;
  } pending;

  bool isPowered = false;
  bool oldPlaneValid = false;  // DTM1 holds what's on the glass
  bool firstFullDone = false;  // first FULL after begin() gets an extra conditioning pass

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
  void loadBank(uint8_t cdi0, const LutBank& bank);
  void fullScreenWindowIn();
  uint32_t powerOnIfNeeded();
  uint32_t runWaveform();
  void reset();
  uint32_t waitBusyTwoPhase(uint32_t startTimeoutMs = 1000, uint32_t workTimeoutMs = 30000);
};
