#!/usr/bin/env python3
"""Render a Markdown document into 1-bit e-paper pages for the Xteink X3.

PROTOTYPE (stage 12, step 1): writes PNG pages so layout choices (portrait vs
landscape, tables, code) can be judged before any firmware exists. The glyphs
are rendered exactly like tools/fontconvert.py / the device: FreeType, hinted,
1-bit (FT_LOAD_TARGET_MONO), so these PNGs ARE what the X3 will show.

Usage:
  pip install freetype-py pillow markdown-it-py
  mdpages.py docs/lesson-05-power.md out/ --orient portrait|landscape

Layout model: a list of blocks (heading, paragraph, list item, code, table,
rule); each becomes a list of "rows" (a line of text runs, or a table row),
and rows are packed onto pages. Table rows never split; the header repeats.
"""
import argparse
import os
import struct

import freetype
from markdown_it import MarkdownIt
from PIL import Image

FONT_DIR = "/usr/share/fonts/noto/"
BEZEL = 3          # px hidden on every side (measured, stage 2b)
MARGIN = 22        # text margin inside the visible area
FOOTER_H = 30
# Characters missing from a font are taken from these, in order (pages are
# images, so unlike the device's cp1252 fonts any Unicode character is fine).
FALLBACKS = ["NotoSansMath-Regular.ttf", "NotoSansSymbols2-Regular.ttf", "NotoSansSymbols-Regular.ttf"]
MISSING = set()  # characters no font had -> reported at the end


class Font:
    """One face at one pixel size; glyphs rendered hinted 1-bit and cached."""

    def __init__(self, file, px):
        self.face = freetype.Face(FONT_DIR + file)
        self.face.set_pixel_sizes(0, px)
        self.fallbacks = []
        for fb in FALLBACKS:
            f = freetype.Face(FONT_DIR + fb)
            f.set_pixel_sizes(0, px)
            self.fallbacks.append(f)
        self.px = px
        self.line_h = self.face.size.height >> 6
        self.ascent = self.face.size.ascender >> 6
        self.cache = {}

    def glyph(self, ch):
        g = self.cache.get(ch)
        if g is None:
            face = self.face
            if face.get_char_index(ord(ch)) == 0 and ch not in "\n":
                face = next((f for f in self.fallbacks if f.get_char_index(ord(ch))), self.face)
                if face is self.face:
                    MISSING.add(ch)
            face.load_char(ch, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
            gl = face.glyph
            bm = gl.bitmap
            rows = [bytes(bm.buffer[y * bm.pitch:(y + 1) * bm.pitch]) for y in range(bm.rows)]
            g = (rows, bm.width, gl.bitmap_left, gl.bitmap_top, gl.advance.x >> 6)
            self.cache[ch] = g
        return g

    def width(self, text):
        return sum(self.glyph(c)[4] for c in text)

    def draw(self, px, x, y_base, text, ink=0):
        """Draw `text` with its baseline at y_base; px = PIL pixel access."""
        for c in text:
            rows, w, left, top, adv = self.glyph(c)
            for yy, row in enumerate(rows):
                for xx in range(w):
                    if (row[xx >> 3] >> (7 - (xx & 7))) & 1:
                        px[x + left + xx, y_base - top + yy] = ink
            x += adv
        return x


class Surface:
    """Pillow pixel access + the image size (draw() needs both for clipping)."""

    def __init__(self, img):
        self.pix = img.load()
        self.W, self.H = img.size
        self.clipped = 0  # pixels that fell off the page = a layout overflow to report

    def __setitem__(self, xy, v):
        x, y = xy
        if 0 <= x < self.W and 0 <= y < self.H:
            self.pix[xy] = v
        else:
            self.clipped += 1


class Fonts:
    def __init__(self):
        self.body = Font("NotoSerif-Regular.ttf", 28)      # settled in stage 9
        self.bold = Font("NotoSerif-Bold.ttf", 28)
        self.h1 = Font("NotoSerif-Bold.ttf", 36)
        self.h2 = Font("NotoSerif-Bold.ttf", 31)
        self.h3 = Font("NotoSerif-Bold.ttf", 28)
        # Smaller faces stay >= 24 px: at ~257 ppi, 21-22 px (~6 pt) is too small in the hand.
        self.code = Font("NotoSansMono-Regular.ttf", 24)
        self.icode = Font("NotoSansMono-Regular.ttf", 25)  # `inline code` inside 28 px text
        self.cell = Font("NotoSans-Regular.ttf", 25)
        self.cellb = Font("NotoSans-Bold.ttf", 25)
        self.cellc = Font("NotoSansMono-Regular.ttf", 24)
        self.small = Font("NotoSans-Regular.ttf", 18)


# ------------------------------------------------------------ markdown -> blocks

def inline_runs(tok, base="body"):
    """markdown-it inline token -> [(text, style)], style in body/bold/code."""
    runs, bold = [], False
    for ch in tok.children or []:
        if ch.type == "text":
            runs.append((ch.content, "bold" if bold else base))
        elif ch.type == "code_inline":
            runs.append((ch.content, "code"))
        elif ch.type == "strong_open":
            bold = True
        elif ch.type == "strong_close":
            bold = False
        elif ch.type in ("softbreak", "hardbreak"):
            runs.append((" ", base))
        elif ch.type == "link_open" or ch.type == "link_close":
            pass  # keep the link text only
        elif ch.content:
            runs.append((ch.content, "bold" if bold else base))
    return runs


def parse(md_text):
    md = MarkdownIt("commonmark").enable("table")
    toks = md.parse(md_text)
    blocks, i, list_depth, item_prefix = [], 0, 0, []
    while i < len(toks):
        t = toks[i]
        if t.type == "heading_open":
            blocks.append(("heading", int(t.tag[1]), inline_runs(toks[i + 1], "bold")))
            i += 3
            continue
        if t.type in ("bullet_list_open", "ordered_list_open"):
            list_depth += 1
            item_prefix.append([t.type == "ordered_list_open", 0])
        elif t.type in ("bullet_list_close", "ordered_list_close"):
            list_depth -= 1
            item_prefix.pop()
        elif t.type == "list_item_open":
            ordered = item_prefix[-1]
            ordered[1] += 1
            marker = f"{ordered[1]}." if ordered[0] else "•"
            # first paragraph of the item carries the marker
            j = i + 1
            while toks[j].type != "inline":
                j += 1
            blocks.append(("item", list_depth, marker, inline_runs(toks[j])))
            i = j + 2
            continue
        elif t.type == "paragraph_open" and list_depth == 0:
            blocks.append(("para", inline_runs(toks[i + 1])))
            i += 3
            continue
        elif t.type in ("fence", "code_block"):
            blocks.append(("code", t.content.rstrip("\n")))
        elif t.type == "hr":
            blocks.append(("rule",))
        elif t.type == "table_open":
            rows, j, cur = [], i + 1, None
            while toks[j].type != "table_close":
                if toks[j].type == "tr_open":
                    cur = []
                elif toks[j].type in ("th_open", "td_open"):
                    cur.append((toks[j].type == "th_open", inline_runs(toks[j + 1], "body")))
                elif toks[j].type == "tr_close":
                    rows.append(cur)
                j += 1
            blocks.append(("table", rows))
            i = j + 1
            continue
        i += 1
    return blocks


# ------------------------------------------------------------ wrapping

def run_font(F, style, in_table=False):
    if in_table:
        return {"bold": F.cellb, "code": F.cellc}.get(style, F.cell)
    return {"bold": F.bold, "code": F.icode}.get(style, F.body)


def wrap_runs(F, runs, width, in_table=False, first_indent=0, font=None):
    """Greedy word wrap of styled runs -> lines of [(text, font)].

    A "word" is a list of styled fragments with no space inside, so "**meter**."
    stays together; spaces appear only where the source text has them.
    `font` forces one font for everything (headings).
    """
    words, cur = [], []
    for text, style in runs:
        f = font or run_font(F, style, in_table)
        for k, part in enumerate(text.split(" ")):
            if k > 0 and cur:  # a space in the source ends the word
                words.append(cur)
                cur = []
            if part:
                if cur and cur[-1][1] is f:
                    cur[-1] = (cur[-1][0] + part, f)
                else:
                    cur.append((part, f))
    if cur:
        words.append(cur)

    lines, line, x = [], [], first_indent
    for w in words:
        ww = sum(f.width(t) for t, f in w)
        sp = w[0][1].width(" ") if line else 0
        if line and x + sp + ww > width:
            lines.append(line)
            line, x, sp = [], 0, 0
        if ww > width:  # longer than a whole line: break it character by character
            chars = [(c, f) for t, f in w for c in t]
            piece, pw = [], 0
            for c, f in chars:
                cw = f.width(c)
                if piece and pw + cw > width:
                    lines.append(merge(piece))
                    piece, pw = [], 0
                piece.append((c, f))
                pw += cw
            w, ww = merge(piece), pw
        frags = list(w)
        if sp:
            frags[0] = (" " + frags[0][0], frags[0][1])
        line.extend(frags)
        x += sp + ww
    if line:
        lines.append(line)
    return lines or [[]]


def merge(chars):
    """[(char, font)...] -> [(text, font)...] joining runs of the same font."""
    out = []
    for c, f in chars:
        if out and out[-1][1] is f:
            out[-1] = (out[-1][0] + c, f)
        else:
            out.append((c, f))
    return out


# ------------------------------------------------------------ layout -> rows

def layout(F, blocks, W):
    """Blocks -> rows. A row is ('text', height, [(dx, text, font)...]) or a table row."""
    rows = []
    usable = W - 2 * (BEZEL + MARGIN)

    def space(h):
        rows.append(("space", h))

    for b in blocks:
        kind = b[0]
        if kind == "heading":
            f = {1: F.h1, 2: F.h2}.get(b[1], F.h3)
            space(f.line_h // 2 if rows else 0)
            title = "".join(t for t, _ in b[2])
            for n, line in enumerate(wrap_runs(F, b[2], usable, font=f)):
                # the first line of a heading carries (level, title) for the XTC chapter table
                rows.append(("text", f.line_h, line_cells(line), "keep", (b[1], title) if n == 0 else None))
            space(6)
        elif kind == "para":
            for line in wrap_runs(F, b[1], usable):
                rows.append(("text", F.body.line_h, line_cells(line), None))
            space(12)
        elif kind == "item":
            depth, marker, runs = b[1], b[2], b[3]
            ind = 26 * depth
            mw = F.body.width(marker + " ")
            for n, line in enumerate(wrap_runs(F, runs, usable - ind - mw)):
                cells = line_cells(line, ind + mw)
                if n == 0:
                    cells.insert(0, (ind, marker, F.body))
                rows.append(("text", F.body.line_h, cells, None))
            space(4)
        elif kind == "code":
            space(4)
            cw = F.code.width("m")
            cols = max(10, (usable - 16) // cw)
            for src in b[1].split("\n"):
                pieces = wrap_code(src, cols)
                for k, p in enumerate(pieces):  # wrapped code lines are marked with an arrow
                    rows.append(("code", F.code.line_h, [(10, ("↪ " if k else "") + p, F.code)], None))
            space(14)
        elif kind == "rule":
            rows.append(("rule", 16))
        elif kind == "table":
            rows.extend(table_rows(F, b[1], usable))
            space(14)
    return rows


def wrap_code(src, cols):
    """Split a code line into <= cols chars, preferring to break after a space."""
    pieces = []
    while len(src) > cols:
        cut = src.rfind(" ", cols // 2, cols)  # a space in the second half of the line
        cut = cut + 1 if cut > 0 else cols
        pieces.append(src[:cut].rstrip())
        src = src[cut:]
    pieces.append(src)
    return pieces


def line_cells(line, x0=0):
    cells, x = [], x0
    for text, f in line:
        cells.append((x, text, f))
        x += f.width(text)
    return cells


def table_rows(F, rows, usable):
    ncol = max(len(r) for r in rows)
    pad = 8
    # natural width of each column = widest cell (single line), capped
    nat = [0] * ncol
    for r in rows:
        for c, (hdr, runs) in enumerate(r):
            txt = "".join(t for t, _ in runs)
            f = F.cellb if hdr else F.cell
            nat[c] = max(nat[c], min(f.width(txt), usable) + 2 * pad)
    total = sum(nat)
    if total > usable:  # shrink wide columns first: give each at least its longest word
        minw = [0] * ncol
        for r in rows:
            for c, (hdr, runs) in enumerate(r):
                for t, st in runs:
                    for w in t.split():
                        minw[c] = max(minw[c], run_font(F, "bold" if hdr else st, True).width(w) + 2 * pad)
        minw = [min(m, n) for m, n in zip(minw, nat)]
        extra = usable - sum(minw)
        if extra >= 0:  # every column gets its longest word; share the rest by "wish"
            flex = [n - m for n, m in zip(nat, minw)]
            widths = [m + extra * fl // max(1, sum(flex)) for m, fl in zip(minw, flex)]
        else:  # even the longest words don't fit: scale to the page, words will break
            widths = [max(48, n * usable // total) for n in nat]
        widths[-1] += usable - sum(widths)  # absorb rounding so the table is exactly page-wide
    else:
        widths = nat
    out, header = [], None
    for ri, r in enumerate(rows):
        cell_lines = []
        for c in range(ncol):
            hdr, runs = r[c] if c < len(r) else (False, [])
            runs = [(t, "bold" if hdr else st) for t, st in runs]
            cell_lines.append(wrap_runs(F, runs, widths[c] - 2 * pad, in_table=True))
        lh = F.cell.line_h
        h = max(len(cl) for cl in cell_lines) * lh + 8
        row = ("trow", h, widths, cell_lines, r and r[0][0], None)
        if ri == 0 and row[4]:
            header = row
        out.append(row)
    return [("table_start", 0, header)] + out + [("table_end", 0)]


# ------------------------------------------------------------ pages

def keep_height(rows, i):
    """Height a heading needs on its page: its own lines + the next 2 content rows."""
    h, j, content = 0, i, 0
    while j < len(rows) and rows[j][0] == "text" and rows[j][3] == "keep":
        h += rows[j][1]
        j += 1
    while j < len(rows) and content < 2:
        if rows[j][0] not in ("table_start", "table_end"):
            h += rows[j][1]
            if rows[j][0] != "space":
                content += 1
        j += 1
    return h


def paginate(F, rows, W, H):
    top = BEZEL + MARGIN
    bottom = H - BEZEL - FOOTER_H
    pages, cur, y, header = [], [], top, None
    for i, r in enumerate(rows):
        kind, h = r[0], r[1]
        if kind == "table_start":
            header = r[2]
            continue
        if kind == "table_end":
            header = None
            continue
        if kind == "space" and not cur:
            continue  # no gap at the top of a page
        # A heading starts a new page unless it fits WITH two lines of what follows.
        first_of_heading = kind == "text" and r[3] == "keep" and not (
            i > 0 and rows[i - 1][0] == "text" and rows[i - 1][3] == "keep")
        need = keep_height(rows, i) if first_of_heading else h
        if y + need > bottom and cur:
            pages.append(cur)
            cur, y = [], top
            if kind == "space":
                continue
            if header is not None and r is not header:  # repeat the table header
                cur.append((y, header))
                y += header[1]
        cur.append((y, r))
        y += h
    if cur:
        pages.append(cur)
    return pages


def render(F, page, W, H, number, total, title):
    img = Image.new("1", (W, H), 1)
    px = Surface(img)
    left = BEZEL + MARGIN
    for y, r in page:
        kind = r[0]
        if kind in ("text", "code"):
            for dx, text, f in r[2]:
                f.draw(px, left + dx, y + f.ascent, text)
        elif kind == "rule":
            for x in range(left, W - left):
                px[x, y + 8] = 0
        elif kind == "trow":
            _, h, widths, cell_lines, is_hdr, _ = r
            x = left
            for c, lines in enumerate(cell_lines):
                for li, line in enumerate(lines):
                    cx = x + 8
                    for text, f in line:
                        cx = f.draw(px, cx, y + 4 + li * F.cell.line_h + F.cell.ascent, text)
                x += widths[c]
            tw = sum(widths)
            for xx in range(left, left + tw):  # rule under each row (double under the header)
                px[xx, y + h - 1] = 0
                if is_hdr:
                    px[xx, y + h - 3] = 0
    foot = f"{title}   {number}/{total}"
    F.small.draw(px, W - BEZEL - MARGIN - F.small.width(foot), H - BEZEL - 9, foot)
    if px.clipped:
        print(f"  WARNING page {number}: {px.clipped} pixels fell outside the page (overflow)")
    return img


# ------------------------------------------------------------ XTC export

def chapters_from(pages, level=2):
    """[(title, first_page)] for headings of `level` (0-based pages)."""
    out = []
    for n, page in enumerate(pages):
        for _, r in page:
            if r[0] == "text" and len(r) > 4 and r[4] and r[4][0] == level:
                out.append((r[4][1], n))
    return out


def write_xtc(path, images, title, chapters, author=""):
    """Xteink XTC: header, title/author, chapters, page table, XTG 1-bit pages.
    Layout from CrossPoint lib/Xtc (XtcTypes.h / XtcParser.cpp). Bit 1 = white."""
    n = len(images)
    W, H = images[0].size
    HEADER, META = 56, 128 + 64
    chap_off = HEADER + META
    table_off = chap_off + 96 * len(chapters)
    data_off = table_off + 16 * n
    page_bytes = [im.tobytes() for im in images]  # PIL mode "1": MSB first, 1 = white
    out = bytearray()
    out += struct.pack("<IBBHBBBBIQQQQII", 0x00435458, 1, 0, n, 0, 1, 0, 1 if chapters else 0, 0,
                       HEADER, table_off, data_off, 0, chap_off if chapters else 0, 0)
    out += title.encode("utf-8")[:127].decode("utf-8", "ignore").encode("utf-8").ljust(128, b"\0")
    out += author.encode("utf-8")[:63].ljust(64, b"\0")
    for k, (name, start) in enumerate(chapters):
        end = (chapters[k + 1][1] - 1) if k + 1 < len(chapters) else n - 1
        end = max(end, start)
        # cut to 79 bytes on a character boundary (never split a UTF-8 sequence)
        nm = name.encode("utf-8")[:79].decode("utf-8", "ignore").encode("utf-8")
        rec = nm.ljust(80, b"\0") + struct.pack("<HH", start + 1, end + 1)
        out += rec.ljust(96, b"\0")
    off = data_off
    for b in page_bytes:
        out += struct.pack("<QIHH", off, 22 + len(b), W, H)
        off += 22 + len(b)
    for b in page_bytes:
        out += struct.pack("<IHHBBIQ", 0x00475458, W, H, 0, 0, len(b), 0) + b
    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def build(markdown, outdir, xtc=None, orient="landscape", F=None):
    """Render one Markdown file: PNG pages in outdir, optionally an .xtc. Returns (pages, xtc size)."""
    W, H = (528, 792) if orient == "portrait" else (792, 528)
    F = F or Fonts()
    text = open(markdown, encoding="utf-8").read()
    title = os.path.splitext(os.path.basename(markdown))[0]
    pages = paginate(F, layout(F, parse(text), W), W, H)
    os.makedirs(outdir, exist_ok=True)
    images = []
    for n, page in enumerate(pages, 1):
        img = render(F, page, W, H, n, len(pages), title)
        img.save(os.path.join(outdir, f"page-{n:03d}.png"))
        images.append(img)
    size = 0
    if xtc:
        h1 = next((r[4][1] for p in pages for _, r in p if r[0] == "text" and len(r) > 4 and r[4] and r[4][0] == 1), title)
        chapters = chapters_from(pages)
        size = write_xtc(xtc, images, h1, chapters)
        print(f"  xtc: {xtc} ({size // 1024} KB, {len(chapters)} chapters, title {h1!r})")
    print(f"{markdown}: {len(pages)} pages ({orient} {W}x{H}) -> {outdir}")
    if MISSING:
        print("  WARNING no font has: " + " ".join(f"U+{ord(c):04X} {c}" for c in sorted(MISSING)))
    return len(pages), size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("markdown")
    ap.add_argument("outdir")
    ap.add_argument("--orient", choices=["portrait", "landscape"], default="landscape")  # chosen for lessons
    ap.add_argument("--xtc", help="also write the pages as an Xteink .xtc file")
    a = ap.parse_args()
    build(a.markdown, a.outdir, a.xtc, a.orient)


if __name__ == "__main__":
    main()
