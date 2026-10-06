// The page that was on screen at sleep, kept in internal flash (the unused
// "spiffs" data partition, written raw: no filesystem). On wake it lets the
// display refresh start before the SD card has finished its ~650 ms cold start.
#pragma once
#include <Arduino.h>

namespace wake_frame {

// What the frame shows; it's only used if this still matches on wake.
struct Meta {
  uint32_t bookCrc;  // CRC32 of the book path
  uint32_t pos;      // byte offset of the first word on the page
  uint8_t fontPx;
};

// Save `len` bytes of frame buffer. ~0.5 s: flash must be erased first.
bool save(const uint8_t* fb, size_t len, const Meta& meta);

// Copy the saved frame into `fb` if it exists, is intact (CRC) and matches `meta`.
bool load(uint8_t* fb, size_t len, const Meta& meta);

// Forget the saved frame (so a stale one is never shown).
void invalidate();

}  // namespace wake_frame
