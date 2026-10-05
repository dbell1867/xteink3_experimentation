// Plain-text book: streams a UTF-8 .txt from the SD card and lays it out in
// pages. Handles Project Gutenberg files (hard-wrapped lines, blank line =
// new paragraph, licence header/footer skipped).
#pragma once
#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <FS.h>

#include <vector>

class TextBook {
 public:
  // Page area in canvas (portrait) coordinates.
  struct Layout {
    const GFXfont* font;
    int left, right;      // text spans [left, right)
    int top, bottom;      // first baseline >= top + ascent; nothing below bottom
    int paragraphGap;     // extra pixels between paragraphs
    int indent;           // first-line indent of a paragraph
  };

  bool open(fs::FS& fs, const char* path);
  const String& title() const { return bookTitle; }

  // Lay out every page once (no drawing) and remember where each starts.
  // Returns the number of pages. (Python: like building a list of offsets.)
  int buildIndex(const Layout& layout);
  int pageCount() const { return pageStarts.size(); }

  // Draw page n (0-based) onto the canvas.
  void drawPage(GFXcanvas1& canvas, const Layout& layout, int n);

 private:
  // One word (already converted to ASCII) or a paragraph break.
  struct Token {
    enum Kind { Word, Paragraph, End } kind;
    uint32_t offset;  // byte offset in the file where it starts
    char text[64];
    uint8_t len;
  };

  File file;
  String bookTitle;
  uint32_t bodyStart = 0, bodyEnd = 0;
  std::vector<uint32_t> pageStarts;  // std::vector = a growable array (Python list)

  // Buffered reader
  uint8_t buf[512];
  uint32_t bufStart = 0;  // file offset of buf[0]
  uint16_t bufLen = 0;
  uint32_t pos = 0;       // offset of the next byte to read

  void seek(uint32_t offset);
  int peekByte();  // -1 at end of body
  int readByte();
  bool startsParagraph(uint32_t offset);
  void appendCodepoint(Token& t, uint32_t cp);
  Token nextToken();
  uint32_t layoutPage(uint32_t start, const Layout& layout, GFXcanvas1* canvas);
  void findBody();
};
