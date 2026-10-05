# Fonts

`include/NotoSerif{28,32,36,40}.h` (body text) and `include/NotoSans20.h` (header, menus) are generated
from **Noto Serif Regular** / **Noto Sans Regular** (v2.015,
© 2022 The Noto Project Authors, https://github.com/notofonts/latin-greek-cyrillic),
licensed under the **SIL Open Font License 1.1** — see `OFL.txt` (from that repo).
The font file declares the OFL in its own metadata (name ID 13).

Regenerate (px em sizes; panel ≈ 257 ppi so 28/32/36/40 px ≈ 8/9/10/11 pt; cp1252 layout 0x20–0xFF):

    pip install freetype-py pillow
    for px in 28 32 36 40; do
      python tools/fontconvert.py /usr/share/fonts/noto/NotoSerif-Regular.ttf $px NotoSerif$px > include/NotoSerif$px.h
    done
    python tools/fontconvert.py /usr/share/fonts/noto/NotoSans-Regular.ttf 20 NotoSans20 > include/NotoSans20.h
