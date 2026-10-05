// UTF-8 -> Windows-1252 (cp1252), the byte layout of all our on-device fonts.
// Adafruit GFX draws one byte per glyph, so every string shown with our fonts
// must be cp1252: ASCII, Latin-1 letters (U+00A0-00FF keep their value) and
// typographic punctuation packed into 0x80-0x9F. Anything else -> '?'.
// Showing raw UTF-8 instead gives "Â·" for "·" (bytes C2 B7) and "â€”" for "—".
#pragma once
#include <Arduino.h>

inline uint8_t toCp1252(uint32_t cp) {
  // cp1252 bytes 0x80..0x9F -> their Unicode code points (0 = unused byte)
  static const uint16_t C1[32] = {
      0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,  // 0x80
      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,       // 0x88
      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,  // 0x90
      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,  // 0x98
  };
  if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) return uint8_t(cp);
  for (int i = 0; i < 32; i++)  // reverse lookup in the 32-entry table
    if (C1[i] == cp) return uint8_t(0x80 + i);
  return '?';
}

// Decode a whole UTF-8 string into cp1252 bytes.
inline String utf8ToCp1252(const char* s) {
  String out;
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(s); *p;) {
    uint32_t cp = *p++;
    int extra = (cp >= 0xF0) ? 3 : (cp >= 0xE0) ? 2 : (cp >= 0xC0) ? 1 : 0;
    if (extra) cp &= 0x3F >> extra;
    while (extra-- > 0 && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
    out += char(toCp1252(cp));
  }
  return out;
}
