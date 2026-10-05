#include "text_book.h"

#include <algorithm>

namespace {
// Width of an ASCII string in pixels, straight from the font's glyph table
// (much faster than getTextBounds, which matters when indexing a whole book).
int textWidth(const GFXfont* font, const char* s, int len) {
  int w = 0;
  for (int i = 0; i < len; i++) {
    const uint8_t c = s[i];
    if (c >= font->first && c <= font->last) w += font->glyph[c - font->first].xAdvance;
  }
  return w;
}

// Search the file for `needle`, starting at `from`. Returns offset or -1.
int32_t findInFile(File& f, const char* needle, uint32_t from, uint32_t limit) {
  const size_t n = strlen(needle);
  char chunk[1024];
  uint32_t at = from;
  while (at < limit) {
    f.seek(at);
    const int got = f.read(reinterpret_cast<uint8_t*>(chunk), sizeof(chunk));
    if (got < (int)n) return -1;
    for (int i = 0; i + (int)n <= got; i++)
      if (memcmp(chunk + i, needle, n) == 0) return at + i;
    at += got - n + 1;  // overlap so a match across chunks isn't missed
  }
  return -1;
}
}  // namespace

bool TextBook::open(fs::FS& fs, const char* path) {
  file = fs.open(path);
  if (!file) return false;
  bookTitle = path;
  bodyStart = 0;
  bodyEnd = file.size();
  pageStarts.clear();
  return true;
}

// Gutenberg files wrap the book in a licence header/footer:
//   "*** START OF THE PROJECT GUTENBERG EBOOK ... ***"  ...  "*** END OF ..."
// and carry "Title: ..." in the header. Plain .txt files have none of this.
void TextBook::findBody() {
  const uint32_t size = file.size();
  bodyStart = 0;
  bodyEnd = size;

  const int32_t start = findInFile(file, "*** START", 0, size);
  if (start >= 0) {
    const int32_t eol = findInFile(file, "\n", start, size);
    bodyStart = eol >= 0 ? eol + 1 : start;

    const int32_t t = findInFile(file, "Title: ", 0, start);
    if (t >= 0) {
      file.seek(t + 7);
      String s;
      for (int c = file.read(); c >= 0 && c != '\r' && c != '\n' && s.length() < 60; c = file.read()) s += char(c);
      bookTitle = s;
    }
  }
  const int32_t end = findInFile(file, "*** END", bodyStart, size);
  if (end >= 0) bodyEnd = end;
}

// ---------------------------------------------------------- byte reader ---

void TextBook::seek(uint32_t offset) {
  pos = offset;
  bufLen = 0;  // force a refill
}

int TextBook::peekByte() {
  if (pos >= bodyEnd) return -1;
  if (pos < bufStart || pos >= bufStart + bufLen) {
    file.seek(pos);
    bufStart = pos;
    bufLen = file.read(buf, sizeof(buf));
    if (bufLen == 0) return -1;
  }
  return buf[pos - bufStart];
}

int TextBook::readByte() {
  const int c = peekByte();
  if (c >= 0) pos++;
  return c;
}

// A page starting at `offset` begins a paragraph if a blank line precedes it.
bool TextBook::startsParagraph(uint32_t offset) {
  if (offset <= bodyStart) return true;
  int newlines = 0;
  for (uint32_t p = offset; p > bodyStart && p + 8 > offset; p--) {
    seek(p - 1);
    const int c = peekByte();
    if (c == '\n') newlines++;
    else if (c != '\r' && c != ' ') break;
  }
  return newlines >= 2;
}

// ---------------------------------------------------------- tokenizer ---

// Map a Unicode code point to the byte our font uses for it. The font is laid
// out as Windows-1252: ASCII, Latin-1 letters (U+00A0-00FF keep their value),
// and typographic punctuation packed into 0x80-0x9F. Anything else -> '?'.
namespace {
uint8_t toCp1252(uint32_t cp);
}

void TextBook::appendCodepoint(Token& t, uint32_t cp) {
  if (t.len < sizeof(t.text) - 1) t.text[t.len++] = char(toCp1252(cp));
}

namespace {
uint8_t toCp1252(uint32_t cp) {
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
}  // namespace

// `first` is a lead byte already consumed; read its continuation bytes.
uint32_t TextBook::readCodepoint(int first) {
  uint32_t cp = first;
  int extra = (first >= 0xF0) ? 3 : (first >= 0xE0) ? 2 : (first >= 0xC0) ? 1 : 0;
  if (extra) cp = first & (0x3F >> extra);
  while (extra-- > 0) {
    const int cc = readByte();
    if (cc < 0) break;
    cp = (cp << 6) | (cc & 0x3F);
  }
  return cp;
}

// Is there a chapter heading paragraph at `offset`? A heading: first word
// CHAPTER or LETTER (any case), at most 3 lines, at most 200 bytes, ended by
// a blank line. That excludes a table of contents (one long paragraph).
bool TextBook::readHeading(uint32_t offset, Heading& h) {
  h = Heading{};
  seek(offset);
  int len = 0;  // chars in the current line
  for (int bytes = 0;; bytes++) {
    const int c = readByte();
    if (bytes > 200) return false;
    if (c < 0 || c == '\n') {
      while (len > 0 && h.lines[h.count][len - 1] == ' ') len--;  // trim right
      if (len == 0) break;  // blank line (or end) = end of the paragraph
      h.lines[h.count][len] = 0;
      if (++h.count == 3) {  // a 4th non-empty line means: not a heading
        seek(pos);
        const int n = peekByte();
        if (n >= 0 && n != '\n' && n != '\r') return false;
        break;
      }
      len = 0;
      if (c < 0) break;
      continue;
    }
    if (c == '\r') continue;
    const uint32_t cp = readCodepoint(c);
    if ((cp == ' ' && len == 0) || cp == '[' || cp == ']') continue;  // trim left, drop caption brackets
    if (len < 62) h.lines[h.count][len++] = char(toCp1252(cp));
  }
  h.end = pos;
  if (h.count == 0) return false;
  const char* l0 = h.lines[0];
  return (strncasecmp(l0, "chapter", 7) == 0 && (l0[7] == ' ' || l0[7] == 0)) ||
         (strncasecmp(l0, "letter", 6) == 0 && (l0[6] == ' ' || l0[6] == 0));
}

TextBook::Token TextBook::nextToken() {
  Token t{};
  // Skip whitespace, counting line breaks: a blank line = new paragraph.
  int newlines = 0;
  for (;;) {
    const int c = peekByte();
    if (c < 0) {
      t.kind = Token::End;
      t.offset = pos;
      return t;
    }
    if (c == '\n') newlines++;
    else if (c != '\r' && c != ' ' && c != '\t') break;
    pos++;
  }
  t.offset = pos;
  if (newlines >= 2) {
    t.kind = Token::Paragraph;
    return t;
  }

  // Read one word, decoding UTF-8 as we go.
  t.kind = Token::Word;
  while (t.len < sizeof(t.text) - 5) {
    const int c = peekByte();
    if (c < 0 || c == ' ' || c == '\r' || c == '\n' || c == '\t') break;
    pos++;
    appendCodepoint(t, readCodepoint(c));
  }
  t.text[t.len] = 0;
  return t;
}

// ------------------------------------------------------------- layout ---

// Lay out one page starting at byte `start`. Draws if `canvas` is given.
// Returns the offset where the next page starts (bodyEnd when finished).
uint32_t TextBook::layoutPage(uint32_t start, const Layout& L, GFXcanvas1* canvas) {
  const GFXfont* font = L.font;
  const int lineH = font->yAdvance;
  const int space = textWidth(font, " ", 1);
  const int ascent = -font->glyph['A' - font->first].yOffset;

  bool indentNext = startsParagraph(start);
  seek(start);
  int y = L.top + ascent;  // baseline of the current line
  int x = L.left;
  bool lineEmpty = true;
  if (canvas) canvas->setFont(font);

  for (;;) {
    const Token t = nextToken();
    if (t.kind == Token::End) return bodyEnd;

    if (t.kind == Token::Paragraph) {
      if (!lineEmpty) {
        y += lineH;
        lineEmpty = true;
      }
      if (y > L.top + ascent) y += L.paragraphGap;  // no gap at the top of a page
      x = L.left;
      indentNext = true;
      if (y > L.bottom) return t.offset;
      continue;
    }

    // First word of a paragraph: is this paragraph a chapter heading? (Only
    // read ahead for "chapter"/"letter" words, so indexing stays fast.)
    if (lineEmpty && indentNext &&
        ((t.len == 7 && strncasecmp(t.text, "chapter", 7) == 0) ||
         (t.len == 6 && strncasecmp(t.text, "letter", 6) == 0))) {
      const uint32_t after = pos;
      Heading h;
      if (readHeading(t.offset, h)) {
        if (y > L.top + ascent) return t.offset;  // chapters start on a new page
        y += lineH;                               // drop the heading a little
        for (int i = 0; i < h.count; i++) {
          const int len = strlen(h.lines[i]);
          const int lw = textWidth(font, h.lines[i], len);
          const int cx = max(L.left, (L.left + L.right - lw) / 2);  // centred
          if (canvas) {
            canvas->setCursor(cx, y);
            canvas->print(h.lines[i]);
          }
          y += lineH;
        }
        y += lineH / 2;  // space below the heading
        if (recordChapters) {
          // "CHAPTER I." + "Down the Rabbit-Hole" -> "I. Down the Rabbit-Hole";
          // a lone "Letter 1" / "CHAPTER II." stays as it is.
          String title = h.lines[0];
          if (h.count > 1) {
            if (strncasecmp(h.lines[0], "chapter ", 8) == 0) title = String(h.lines[0] + 8);
            for (int i = 1; i < h.count; i++) title += String(" ") + h.lines[i];
          }
          chapterList.push_back({t.offset, title});
        }
        seek(h.end);
        x = L.left;
        lineEmpty = true;
        indentNext = true;
        if (y > L.bottom) return h.end;
        continue;
      }
      seek(after);  // not a heading: carry on with this word
    }

    // A word: does it fit on this line?
    const int w = textWidth(font, t.text, t.len);
    if (lineEmpty && indentNext) {
      x = L.left + L.indent;
      indentNext = false;
    }
    const int needed = (lineEmpty ? 0 : space) + w;
    if (!lineEmpty && x + needed > L.right) {
      y += lineH;
      x = L.left;
      lineEmpty = true;
    }
    if (y > L.bottom) return t.offset;  // page full: this word starts the next page

    if (!lineEmpty) x += space;
    if (canvas) {
      canvas->setCursor(x, y);
      canvas->print(t.text);
    }
    x += w;
    lineEmpty = false;
  }
}

int TextBook::buildIndex(const Layout& layout) {
  findBody();
  pageStarts.clear();
  chapterList.clear();
  recordChapters = true;
  uint32_t at = bodyStart;
  // Skip leading blank lines so page 1 doesn't start with a gap.
  seek(at);
  while (peekByte() == '\r' || peekByte() == '\n' || peekByte() == ' ') pos++;
  at = pos;

  while (at < bodyEnd) {
    pageStarts.push_back(at);  // push_back = list.append
    const uint32_t next = layoutPage(at, layout, nullptr);
    if (next <= at) break;     // safety: never loop forever
    at = next;
  }
  recordChapters = false;
  return pageStarts.size();
}

int TextBook::chapterForPage(int page) const {
  int found = -1;
  for (int i = 0; i < (int)chapterList.size(); i++)
    if (pageForOffset(chapterList[i].offset) <= page) found = i;
  return found;
}

int TextBook::pageForOffset(uint32_t offset) const {
  // pageStarts is sorted, so binary-search for the last start <= offset
  // (Python: bisect.bisect_right(pageStarts, offset) - 1).
  const auto it = std::upper_bound(pageStarts.begin(), pageStarts.end(), offset);
  return it == pageStarts.begin() ? 0 : int(it - pageStarts.begin()) - 1;
}

void TextBook::drawPage(GFXcanvas1& canvas, const Layout& layout, int n) {
  if (n < 0 || n >= (int)pageStarts.size()) return;
  layoutPage(pageStarts[n], layout, &canvas);
}

// ------------------------------------------------------------ index cache ---

namespace {
constexpr uint32_t IDX_MAGIC = 0x58444933;  // "3IDX" (v3: chapters, heading layout); bump on format/layout-rule changes

// File layout: header, title bytes, pageCount uint32 offsets, then
// uint32 chapterCount and per chapter {uint32 offset, uint8 len, len bytes}.
struct IdxHeader {
  uint32_t magic;
  uint32_t bookSize;
  uint32_t layoutSig;
  uint32_t bodyStart, bodyEnd;
  uint32_t pageCount;
  uint32_t titleLen;
};
}  // namespace

// Anything that changes where pages break must change this number.
uint32_t TextBook::layoutSignature(const Layout& L) const {
  uint32_t h = 2166136261u;  // FNV-1a hash, fed with every layout input
  auto mix = [&h](uint32_t v) {  // a lambda: a small unnamed function (Python: lambda / nested def)
    for (int i = 0; i < 4; i++) {
      h ^= (v >> (8 * i)) & 0xFF;
      h *= 16777619u;
    }
  };
  mix(L.left); mix(L.right); mix(L.top); mix(L.bottom); mix(L.paragraphGap); mix(L.indent);
  mix(L.font->first); mix(L.font->last); mix(L.font->yAdvance);
  for (int c = L.font->first; c <= L.font->last; c++) mix(L.font->glyph[c - L.font->first].xAdvance);
  return h;
}

bool TextBook::saveIndex(fs::FS& fs, const char* cachePath, const Layout& layout) {
  File f = fs.open(cachePath, FILE_WRITE);  // FILE_WRITE truncates: right for a cache
  if (!f) return false;
  const IdxHeader hdr = {IDX_MAGIC, (uint32_t)file.size(), layoutSignature(layout), bodyStart, bodyEnd,
                         (uint32_t)pageStarts.size(), (uint32_t)bookTitle.length()};
  size_t n = f.write(reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr));
  n += f.write(reinterpret_cast<const uint8_t*>(bookTitle.c_str()), hdr.titleLen);
  n += f.write(reinterpret_cast<const uint8_t*>(pageStarts.data()), pageStarts.size() * sizeof(uint32_t));
  size_t want = sizeof(hdr) + hdr.titleLen + pageStarts.size() * sizeof(uint32_t);
  const uint32_t nch = chapterList.size();
  n += f.write(reinterpret_cast<const uint8_t*>(&nch), 4);
  want += 4;
  for (const Chapter& c : chapterList) {
    const uint8_t len = min<size_t>(c.title.length(), 255);
    n += f.write(reinterpret_cast<const uint8_t*>(&c.offset), 4);
    n += f.write(&len, 1);
    n += f.write(reinterpret_cast<const uint8_t*>(c.title.c_str()), len);
    want += 5 + len;
  }
  f.close();  // buffered writes only reach the card on close (G25g)
  return n == want;
}

bool TextBook::loadIndex(fs::FS& fs, const char* cachePath, const Layout& layout) {
  File f = fs.open(cachePath);
  if (!f) return false;
  IdxHeader hdr;
  bool ok = f.read(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr)) == sizeof(hdr) && hdr.magic == IDX_MAGIC &&
            hdr.bookSize == file.size() && hdr.layoutSig == layoutSignature(layout) && hdr.titleLen < 200 &&
            hdr.pageCount > 0 && hdr.pageCount < 100000;
  if (ok) {
    char title[200];
    ok = f.read(reinterpret_cast<uint8_t*>(title), hdr.titleLen) == hdr.titleLen;
    title[hdr.titleLen] = 0;
    pageStarts.resize(hdr.pageCount);
    ok = ok && f.read(reinterpret_cast<uint8_t*>(pageStarts.data()), hdr.pageCount * sizeof(uint32_t)) ==
                   hdr.pageCount * sizeof(uint32_t);
    uint32_t nch = 0;
    ok = ok && f.read(reinterpret_cast<uint8_t*>(&nch), 4) == 4 && nch < 10000;
    chapterList.clear();
    for (uint32_t i = 0; ok && i < nch; i++) {
      uint32_t off;
      uint8_t len;
      char buf[256];
      ok = f.read(reinterpret_cast<uint8_t*>(&off), 4) == 4 && f.read(&len, 1) == 1 &&
           f.read(reinterpret_cast<uint8_t*>(buf), len) == len;
      buf[len] = 0;
      if (ok) chapterList.push_back({off, String(buf)});
    }
    ok = ok && f.available() == 0;  // nothing left over = the file is what we think
    if (ok) {
      bookTitle = title;
      bodyStart = hdr.bodyStart;
      bodyEnd = hdr.bodyEnd;
    }
  }
  f.close();
  if (!ok) {
    pageStarts.clear();
    chapterList.clear();
  }
  return ok;
}
