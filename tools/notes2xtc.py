#!/usr/bin/env python3
"""Talking points for the X3: a marimo slide deck's speaker notes -> one X3 page per step.

Reads the notebook (deck.py) and its slides layout (layouts/<deck>.slides.json, named in
marimo.App(layout_file=...)). marimo pairs layout entries with cells BY POSITION.
Steps follow reveal.js's linear order: each "slide" starts a new number, a "sub-slide"
is a vertical step, each "fragment" is a step on the same slide, "skip" is ignored.

Each page: "6 · <slide title>", "step k/n", the slide's notes (+ any fragment's own notes),
and "Next ▸ <next step>". Output: an .xtc with one chapter per slide (rendered by mdpages).

Usage: notes2xtc.py ~/Work/Python/Marimo/xteink3_deck/deck.py [--out build/xtc]
"""
import argparse
import ast
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mdpages  # noqa: E402  (renderer + XTC writer)


def cells_and_layout(deck_path):
    src = open(deck_path, encoding="utf-8").read()
    tree = ast.parse(src)
    cells, layout_file = [], None
    for node in tree.body:
        # app = marimo.App(..., layout_file="layouts/deck.slides.json")
        if isinstance(node, ast.Assign) and isinstance(node.value, ast.Call):
            for kw in node.value.keywords:
                if kw.arg == "layout_file" and isinstance(kw.value, ast.Constant):
                    layout_file = kw.value.value
        # @app.cell functions, in file order = notebook order
        if isinstance(node, ast.FunctionDef) and any(
                isinstance(d, ast.Attribute) and d.attr == "cell" or
                isinstance(d, ast.Call) and getattr(d.func, "attr", "") == "cell" for d in node.decorator_list):
            cells.append(ast.get_source_segment(src, node))
    if not layout_file:
        sys.exit(f"{deck_path}: no layout_file in marimo.App(...) - save the slides layout in marimo first")
    path = os.path.join(os.path.dirname(os.path.abspath(deck_path)), layout_file)
    layout = json.load(open(path, encoding="utf-8"))
    if layout.get("type") != "slides":
        sys.exit(f"{path}: not a slides layout")
    lcells = layout["data"].get("cells", [])
    if len(lcells) != len(cells):
        print(f"WARNING: layout has {len(lcells)} cells, notebook {len(cells)} - notes may be shifted")
    return cells, lcells


def title_of(cell_src):
    """First Markdown heading in the cell's string literals, else their first text line.

    Uses the AST, so quotes inside a heading and headings that don't start a
    source line (mo.vstack([mo.md("## ..."), ...])) are both handled."""
    try:
        tree = ast.parse(cell_src)
    except SyntaxError:
        return ""
    strings = sorted((n.lineno, n.col_offset, n.value) for n in ast.walk(tree)
                     if isinstance(n, ast.Constant) and isinstance(n.value, str))
    texts = [v for _, _, v in strings]
    for t in texts:
        m = re.search(r"^\s*#{1,6}\s+(.+?)\s*$", t, re.M)
        if m:
            return re.sub(r"[*`]", "", m.group(1))
    for t in texts:
        for line in t.splitlines():
            if line.strip() and not line.strip().startswith(("graph", "```")):
                return re.sub(r"[*`_]", "", line.strip().lstrip("- ").strip())
    return ""


def steps(cells, lcells):
    """[(slide_no, slide_title, kind, label, notes)] in reveal.js linear order."""
    out, n, cur_title, cur_notes = [], 0, "", ""
    for i, src in enumerate(cells):
        lc = lcells[i] if i < len(lcells) else {}
        kind = lc.get("type", "slide")
        notes = lc.get("speakerNotes", "").strip()
        if kind == "skip":
            continue
        if kind == "slide" or (kind == "sub-slide" and n == 0):
            n += 1
            cur_title, cur_notes = title_of(src) or f"Slide {n}", notes
            out.append((n, cur_title, "slide", cur_title, cur_notes))
        elif kind == "sub-slide":
            t = title_of(src) or "(sub-slide)"
            out.append((n, cur_title, "sub-slide", t, notes))
        elif kind == "fragment":
            t = title_of(src) or "(fragment)"
            out.append((n, cur_title, "fragment", t, "\n".join(x for x in (cur_notes, notes) if x)))
    return out


def fit(font, text, width):
    """Shorten text with an ellipsis until it fits `width` pixels."""
    if font.width(text) <= width:
        return text
    while text and font.width(text + "…") > width:
        text = text[:-1]
    return text + "…"


def render_step(F, hfont, k, seq, deck, W=792, H=528):
    """One talk page: a single header line, the notes as large as possible,
    'Next' in the footer. Returns (image, overflowed)."""
    from PIL import Image
    n, slide_title, kind, label, notes = seq[k]
    on_slide = [x for x in seq if x[0] == n]
    pos = on_slide.index(seq[k]) + 1
    img = Image.new("1", (W, H), 1)
    px = mdpages.Surface(img)
    L = mdpages.BEZEL + mdpages.MARGIN
    R = W - L

    # header: "6 · Ghosting ..." left, "step 2/4" right, one line
    tag = f"step {pos}/{len(on_slide)}"
    base = L + hfont.ascent
    F.cell.draw(px, R - F.cell.width(tag), base, tag)
    hfont.draw(px, L, base, fit(hfont, f"{n} · {slide_title}", R - L - F.cell.width(tag) - 20))
    y = L + hfont.line_h + 2
    for x in range(L, R):
        px[x, y] = 0
    y += 10
    if kind in ("fragment", "sub-slide"):  # what this click shows
        what = ("▸ reveal: " if kind == "fragment" else "↓ ") + label
        F.cellb.draw(px, L, y + F.cellb.ascent, fit(F.cellb, what, R - L))
        y += F.cellb.line_h + 6

    # notes (Markdown bullets) at body size
    bottom = H - mdpages.BEZEL - 34
    overflow = False
    for r in mdpages.layout(F, mdpages.parse(notes or "*(no notes)*"), W):
        if r[0] == "space":
            y += r[1]
            continue
        if r[0] not in ("text", "code"):
            continue
        if y + r[1] > bottom:
            overflow = True
            break
        for dx, text, f in r[2]:
            f.draw(px, L + dx, y + f.ascent, text)
        y += r[1]

    # footer: what comes next, and where we are in the deck
    nxt = seq[k + 1] if k + 1 < len(seq) else None
    nxt_text = "Next ▸ end of deck" if not nxt else (
        f"Next ▸ {nxt[0]} · {nxt[3]}" if nxt[0] != n else f"Next ▸ {nxt[2]}: {nxt[3]}")
    where = f"{deck}  {k + 1}/{len(seq)}"
    fb = H - mdpages.BEZEL - 9
    F.small.draw(px, R - F.small.width(where), fb, where)
    F.small.draw(px, L, fb, fit(F.small, nxt_text, R - L - F.small.width(where) - 24))
    return img, overflow


def build(deck, xtc, preview, name=None, F=None):
    """Render a deck's talking points: one page per step -> xtc, PNGs in preview.
    Returns the steps whose notes were cut off (empty list = all fit)."""
    cells, lcells = cells_and_layout(deck)
    seq = steps(cells, lcells)
    name = name or os.path.basename(os.path.dirname(os.path.abspath(deck)))

    F = F or mdpages.Fonts()
    hfont = mdpages.Font("NotoSerif-Bold.ttf", 30)
    images, chapters, overflow = [], [], []
    for k in range(len(seq)):
        img, over = render_step(F, hfont, k, seq, f"talk: {name}")
        if over:
            overflow.append(k + 1)
        images.append(img)
        if seq[k][2] == "slide":
            chapters.append((f"{seq[k][0]} · {seq[k][1]}", k))
    os.makedirs(os.path.dirname(xtc) or ".", exist_ok=True)
    size = mdpages.write_xtc(xtc, images, f"Talk: {name}", chapters)
    os.makedirs(preview, exist_ok=True)
    for k, img in enumerate(images, 1):
        img.save(os.path.join(preview, f"page-{k:03d}.png"))
    print(f"{deck}: {len(cells)} cells -> {len(seq)} steps on {seq[-1][0] if seq else 0} slides")
    print(f"  {xtc} ({size // 1024} KB), previews in {preview}")
    if overflow:
        print(f"  WARNING: notes cut off on steps {overflow} - shorten them (one page per step)")
    return overflow


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("deck")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(HERE), "build", "xtc"))
    a = ap.parse_args()
    deck_dir = os.path.basename(os.path.dirname(os.path.abspath(a.deck)))
    build(a.deck, os.path.join(a.out, f"talk-{deck_dir}.xtc"),
          os.path.join(os.path.dirname(a.out), "pages", f"talk-{deck_dir}"))


if __name__ == "__main__":
    main()
