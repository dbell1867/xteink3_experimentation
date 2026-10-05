// Xteink XTC book: pre-rendered 1-bit pages (written by tools/mdpages.py).
// Format from CrossPoint lib/Xtc: 56-byte header, title/author, 96-byte chapter
// records, a 16-byte page table entry per page, then XTG pages (22-byte header
// + row-major 1-bit bitmap, MSB first, bit 1 = white).
//
// Only pages matching the panel's native layout (792x528, 1-bit) are shown:
// then a page is copied from the SD card straight into the frame buffer.
#pragma once
#include <Arduino.h>
#include <FS.h>

#include <vector>

class XtcBook {
 public:
  struct Chapter {
    int page;      // 0-based first page
    String title;  // UTF-8 as stored in the file
  };

  // Opens and validates the header. On failure, error() says why.
  bool open(fs::FS& fs, const char* path);
  void close() { file.close(); }

  int pageCount() const { return pages; }
  const String& title() const { return bookTitle; }
  const std::vector<Chapter>& chapters() const { return chapterList; }
  int chapterForPage(int page) const;  // index into chapters(), -1 before the first

  // Copy page n into fb (exactly len bytes). False if the page isn't a
  // native-layout 1-bit page or the read fails.
  bool readPage(int n, uint8_t* fb, size_t len, uint16_t width, uint16_t height);

  const char* error() const { return err; }

  // Page count from the header only (for the Books list), 0 if not an XTC.
  static int peekPageCount(fs::FS& fs, const char* path);

 private:
  File file;
  int pages = 0;
  uint64_t tableOffset = 0;
  String bookTitle;
  std::vector<Chapter> chapterList;
  const char* err = "";
};
