#!/usr/bin/env python3
"""Independent XTC reader: print header/chapters and (optionally) check every page
bit-for-bit against a folder of PNGs. Written from CrossPoint's XtcParser, not from
our writer, so it can disagree with it.  Usage: xtcinfo.py book.xtc [png_dir]"""
import glob
import struct
import sys

d = open(sys.argv[1], "rb").read()
magic, vmaj, vmin, n, rd, hasmeta, hasthumb, haschap, cur = struct.unpack_from("<IBBHBBBBI", d, 0)
meta_off, table_off, data_off, thumb_off = struct.unpack_from("<QQQQ", d, 0x10)
chap_off = struct.unpack_from("<Q", d, 0x30)[0]  # CrossPoint reads 8 bytes here
assert magic == 0x00435458, f"bad magic {magic:#x}"
title = d[0x38:0x38 + 128].split(b"\0")[0].decode()
print(f"XTC v{vmaj}.{vmin}  pages {n}  title {title!r}  chapters={haschap}  size {len(d)} B")
if haschap and chap_off:
    end = table_off if table_off > chap_off else data_off
    for k in range((end - chap_off) // 96):
        rec = d[chap_off + 96 * k: chap_off + 96 * (k + 1)]
        name = rec[:80].split(b"\0")[0].decode()
        s, e = struct.unpack_from("<HH", rec, 0x50)
        print(f"  ch {k + 1:2}: pages {s:3}-{e:3}  {name}")
bad = 0
pngs = sorted(glob.glob(sys.argv[2] + "/page-*.png")) if len(sys.argv) > 2 else []
for p in range(n):
    off, size, w, h = struct.unpack_from("<QIHH", d, table_off + 16 * p)
    pm, pw, ph, cm, comp, ds, md5 = struct.unpack_from("<IHHBBIQ", d, off)
    ok = pm == 0x00475458 and (pw, ph) == (w, h) and ds == ((w + 7) // 8) * h and size == 22 + ds
    if pngs:
        from PIL import Image
        ok = ok and Image.open(pngs[p]).convert("1").tobytes() == d[off + 22: off + 22 + ds]
    bad += not ok
print(f"pages checked: {n}, mismatches: {bad}" + (" (bit-for-bit vs PNGs)" if pngs else ""))
