#!/usr/bin/env python3
"""Host simulation of src/text_book.cpp pagination (a line-by-line port over raw
bytes) so page counts and chapter lists can be predicted before flashing.

Usage: pagesim.py include/NotoSerif28.h books/alice.txt [more books...]
Keep in step with text_book.cpp: layout rules here must match the firmware.
"""
import re
import sys

C1 = [0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,
      0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A,
      0x0153, 0, 0x017E, 0x0178]


def to_cp1252(cp):
    if cp < 0x80 or 0xA0 <= cp <= 0xFF:
        return cp
    return 0x80 + C1.index(cp) if cp in C1 else ord('?')


def load_font(header):
    h = open(header).read()
    g = re.findall(r'\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+)\},\s*//\s*0x([0-9A-F]{2})', h)
    adv = {int(b, 16): int(a) for (_, _, _, a, _, _, b) in g}
    ascent = -[int(yo) for (_, _, _, _, _, yo, b) in g if b == '41'][0]
    yadv = int(re.search(r'0x20, 0xFF, (\d+)\}', h).group(1))
    return adv, ascent, yadv


class Book:
    def __init__(self, data):
        self.d = data
        s = data.find(b'*** START')
        self.body_start = data.find(b'\n', s) + 1 if s >= 0 else 0
        e = data.find(b'*** END', self.body_start)
        self.body_end = e if e >= 0 else len(data)
        self.pos = 0

    def peek(self):
        return self.d[self.pos] if self.pos < self.body_end else -1

    def read(self):
        c = self.peek()
        if c >= 0:
            self.pos += 1
        return c

    def codepoint(self, first):
        extra = 3 if first >= 0xF0 else 2 if first >= 0xE0 else 1 if first >= 0xC0 else 0
        cp = first & (0x3F >> extra) if extra else first
        for _ in range(extra):
            cc = self.read()
            if cc < 0:
                break
            cp = (cp << 6) | (cc & 0x3F)
        return cp

    def starts_paragraph(self, off):
        if off <= self.body_start:
            return True
        nl, p = 0, off
        while p > self.body_start and p + 8 > off:
            c = self.d[p - 1]
            if c == 10:
                nl += 1
            elif c not in (13, 32):
                break
            p -= 1
        return nl >= 2

    def token(self):
        nl = 0
        while True:
            c = self.peek()
            if c < 0:
                return ('E', self.pos, b'')
            if c == 10:
                nl += 1
            elif c not in (13, 32, 9):
                break
            self.pos += 1
        off = self.pos
        if nl >= 2:
            return ('P', off, b'')
        out = bytearray()
        while len(out) < 59:
            c = self.peek()
            if c < 0 or c in (32, 13, 10, 9):
                break
            self.pos += 1
            out.append(to_cp1252(self.codepoint(c)))
        return ('W', off, bytes(out))

    def heading(self, off):
        self.pos = off
        lines, cur, n = [], bytearray(), 0
        while True:
            c = self.read()
            if n > 200:
                return None
            n += 1
            if c < 0 or c == 10:
                cur = cur.rstrip(b' ')
                if not cur:
                    break
                lines.append(bytes(cur))
                if len(lines) == 3:
                    nx = self.peek()
                    if nx >= 0 and nx not in (10, 13):
                        return None
                    break
                cur = bytearray()
                if c < 0:
                    break
                continue
            if c == 13:
                continue
            cp = self.codepoint(c)
            if (cp == 32 and not cur) or cp in (0x5B, 0x5D):
                continue
            if len(cur) < 62:
                cur.append(to_cp1252(cp))
        if not lines:
            return None
        l0 = lines[0].lower()
        ok = (l0.startswith(b'chapter') and (len(l0) == 7 or l0[7:8] == b' ')) or \
             (l0.startswith(b'letter') and (len(l0) == 6 or l0[6:7] == b' '))
        return (lines, self.pos) if ok else None


def paginate(data, font, L=(28, 500, 58, 748, 10, 24)):
    adv, ascent, lineH = font
    left, right, top, bottom, gap, indent = L
    W = lambda s: sum(adv.get(c, 0) for c in s)
    space = W(b' ')
    b = Book(data)
    b.pos = b.body_start
    while b.peek() in (13, 10, 32):
        b.pos += 1
    at, pages, chapters = b.pos, [], []

    def layout(start, record):
        ind = b.starts_paragraph(start)
        b.pos = start
        y, x, empty = top + ascent, left, True
        while True:
            kind, off, text = b.token()
            if kind == 'E':
                return b.body_end
            if kind == 'P':
                if not empty:
                    y += lineH
                    empty = True
                if y > top + ascent:
                    y += gap
                x, ind = left, True
                if y > bottom:
                    return off
                continue
            if empty and ind and text.lower() in (b'chapter', b'letter'):
                after = b.pos
                h = b.heading(off)
                if h:
                    lines, end = h
                    if y > top + ascent:
                        return off
                    y += lineH * (len(lines) + 1) + lineH // 2
                    if record:
                        t = lines[0].decode('cp1252')
                        if len(lines) > 1:
                            if t.lower().startswith('chapter '):
                                t = t[8:]
                            t += ' ' + ' '.join(l.decode('cp1252') for l in lines[1:])
                        chapters.append((off, t))
                    b.pos = end
                    x, empty, ind = left, True, True
                    if y > bottom:
                        return end
                    continue
                b.pos = after
            w = W(text)
            if empty and ind:
                x, ind = left + indent, False
            if not empty and x + space + w > right:
                y += lineH
                x, empty = left, True
            if y > bottom:
                return off
            if not empty:
                x += space
            x += w
            empty = False

    while at < b.body_end:
        pages.append(at)
        nxt = layout(at, True)
        if nxt <= at:
            break
        at = nxt
    return pages, chapters


if __name__ == '__main__':
    font = load_font(sys.argv[1])
    for path in sys.argv[2:]:
        pages, ch = paginate(open(path, 'rb').read(), font)
        import bisect
        print(f"{path}: {len(pages)} pages, {len(ch)} chapters")
        for off, t in ch[:3] + ch[-1:]:
            print(f"   p.{bisect.bisect_right(pages, off):4d}  {t}")
