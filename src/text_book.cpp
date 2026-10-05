#include "text_book.h"

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

// Map a Unicode code point to ASCII the font can draw.
void TextBook::appendCodepoint(Token& t, uint32_t cp) {
  const char* s = nullptr;
  char one[2] = {0, 0};
  switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: s = "'"; break;    // curly single quotes
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: s = "\""; break;   // curly double quotes
    case 0x2013: s = "-"; break;                                          // en dash
    case 0x2014: case 0x2015: s = "--"; break;                            // em dash
    case 0x2026: s = "..."; break;                                        // ellipsis
    case 0x2122: s = "(TM)"; break;
    case 0x2022: s = "*"; break;
    default:
      if (cp < 0x80) {
        one[0] = char(cp);
      } else if (cp >= 0xC0 && cp <= 0xFF) {  // Latin-1 letters: drop the accent
        one[0] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy"[cp - 0xC0];
      } else {
        one[0] = '?';
      }
      s = one;
  }
  for (; *s && t.len < sizeof(t.text) - 1; s++) t.text[t.len++] = *s;
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
    uint32_t cp = c;
    int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    if (extra) cp = c & (0x3F >> extra);
    while (extra-- > 0) {
      const int cc = readByte();
      if (cc < 0) break;
      cp = (cp << 6) | (cc & 0x3F);
    }
    appendCodepoint(t, cp);
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
  return pageStarts.size();
}

void TextBook::drawPage(GFXcanvas1& canvas, const Layout& layout, int n) {
  if (n < 0 || n >= (int)pageStarts.size()) return;
  layoutPage(pageStarts[n], layout, &canvas);
}

// ------------------------------------------------------------ index cache ---

namespace {
constexpr uint32_t IDX_MAGIC = 0x58444931;  // "1IDX" little-endian; bump the digit on format changes

// File layout: header, title bytes, then pageCount uint32 offsets.
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
  f.close();  // buffered writes only reach the card on close (G25g)
  return n == sizeof(hdr) + hdr.titleLen + pageStarts.size() * sizeof(uint32_t);
}

bool TextBook::loadIndex(fs::FS& fs, const char* cachePath, const Layout& layout) {
  File f = fs.open(cachePath);
  if (!f) return false;
  IdxHeader hdr;
  bool ok = f.read(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr)) == sizeof(hdr) && hdr.magic == IDX_MAGIC &&
            hdr.bookSize == file.size() && hdr.layoutSig == layoutSignature(layout) && hdr.titleLen < 200 &&
            hdr.pageCount > 0 && hdr.pageCount < 100000 &&
            f.size() == sizeof(hdr) + hdr.titleLen + hdr.pageCount * sizeof(uint32_t);
  if (ok) {
    char title[200];
    ok = f.read(reinterpret_cast<uint8_t*>(title), hdr.titleLen) == hdr.titleLen;
    title[hdr.titleLen] = 0;
    pageStarts.resize(hdr.pageCount);
    ok = ok && f.read(reinterpret_cast<uint8_t*>(pageStarts.data()), hdr.pageCount * sizeof(uint32_t)) ==
                   hdr.pageCount * sizeof(uint32_t);
    if (ok) {
      bookTitle = title;
      bodyStart = hdr.bodyStart;
      bodyEnd = hdr.bodyEnd;
    }
  }
  f.close();
  if (!ok) pageStarts.clear();
  return ok;
}
