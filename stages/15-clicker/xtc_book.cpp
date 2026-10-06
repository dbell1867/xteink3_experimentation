#include "xtc_book.h"

#include "cp1252.h"

namespace {
constexpr uint32_t XTC_MAGIC = 0x00435458;  // "XTC\0" (1-bit); "XTCH" (2-bit) is not supported
constexpr uint32_t XTG_MAGIC = 0x00475458;  // "XTG\0" 1-bit page

// Little-endian readers (the ESP32-C3 is little-endian, but say what we mean).
uint16_t u16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t u32(const uint8_t* p) { return u16(p) | (uint32_t(u16(p + 2)) << 16); }
uint64_t u64(const uint8_t* p) { return u32(p) | (uint64_t(u32(p + 4)) << 32); }
}  // namespace

int XtcBook::peekPageCount(fs::FS& fs, const char* path) {
  File f = fs.open(path);
  uint8_t h[8];
  const bool ok = f && f.read(h, 8) == 8 && u32(h) == XTC_MAGIC;
  f.close();
  return ok ? u16(h + 6) : 0;
}

bool XtcBook::open(fs::FS& fs, const char* path) {
  file = fs.open(path);
  pages = 0;
  chapterList.clear();
  if (!file) {
    err = "cannot open file";
    return false;
  }
  uint8_t h[56];
  if (file.read(h, sizeof(h)) != sizeof(h)) {
    err = "file too short";
    return false;
  }
  if (u32(h) != XTC_MAGIC) {
    err = u32(h) == 0x48435458 ? "2-bit XTCH not supported" : "not an XTC file";
    return false;
  }
  pages = u16(h + 0x06);
  const bool hasChapters = h[0x0B] == 1;
  tableOffset = u64(h + 0x18);
  const uint64_t dataOffset = u64(h + 0x20);
  const uint64_t chapterOffset = u64(h + 0x30);  // CrossPoint reads 8 bytes here
  if (pages == 0 || tableOffset == 0 || tableOffset + 16ull * pages > file.size()) {
    err = "corrupt header";
    pages = 0;
    return false;
  }

  char t[129] = {0};  // title: 128 bytes at 0x38
  file.seek(0x38);
  file.read(reinterpret_cast<uint8_t*>(t), 128);
  bookTitle = utf8ToCp1252(t);  // stored UTF-8; our fonts are cp1252

  if (hasChapters && chapterOffset >= 56) {
    const uint64_t end = tableOffset > chapterOffset ? tableOffset : dataOffset;
    const int count = int((end - chapterOffset) / 96);
    file.seek(chapterOffset);
    for (int i = 0; i < count; i++) {
      uint8_t rec[96];
      if (file.read(rec, 96) != 96) break;
      char name[81] = {0};
      memcpy(name, rec, 80);
      const int start = u16(rec + 0x50);  // 1-based
      if (name[0] == 0 && start == 0) break;
      if (start >= 1 && start <= pages) chapterList.push_back({start - 1, utf8ToCp1252(name)});
    }
  }
  err = "";
  return true;
}

int XtcBook::chapterForPage(int page) const {
  int found = -1;
  for (int i = 0; i < (int)chapterList.size(); i++)
    if (chapterList[i].page <= page) found = i;
  return found;
}

bool XtcBook::readPage(int n, uint8_t* fb, size_t len, uint16_t width, uint16_t height) {
  if (n < 0 || n >= pages) return false;
  uint8_t e[16];
  if (!file.seek(tableOffset + 16ull * n) || file.read(e, 16) != 16) {
    err = "page table read failed";
    return false;
  }
  const uint64_t off = u64(e);
  uint8_t ph[22];
  if (!file.seek(off) || file.read(ph, 22) != 22) {
    err = "page read failed";
    return false;
  }
  if (u32(ph) != XTG_MAGIC || u16(ph + 4) != width || u16(ph + 6) != height || ph[9] != 0 || u32(ph + 10) != len) {
    err = "page is not 792x528 1-bit uncompressed";
    return false;
  }
  return file.read(fb, len) == len;  // straight into the frame buffer
}
