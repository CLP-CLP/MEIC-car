#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Decode each visible glyph in the new photo into a binary bitmap by sampling
the text bands, then print them for comparison with the repo font."""
import numpy as np
from PIL import Image

im = Image.open("IMG_20261012_232417..jpg").convert("L")
a = np.asarray(im).astype(float)

# Panel area (from band analysis): content lives in x 40..660, y 20..350
def dump(name, x0, x1, y0, y1, cols=20, rows=20):
    print(f"\n=== {name}  x {x0}..{x1} y {y0}..{y1} ===")
    for r in range(rows):
        yy0 = y0 + (y1 - y0) * r // rows
        yy1 = max(yy0 + 1, y0 + (y1 - y0) * (r + 1) // rows)
        line = ""
        for c in range(cols):
            xx0 = x0 + (x1 - x0) * c // cols
            xx1 = max(xx0 + 1, x0 + (x1 - x0) * (c + 1) // cols)
            f = (a[yy0:yy1, xx0:xx1] > 120).mean()
            line += "#" if f >= 0.45 else ("+" if f >= 0.18 else ".")
        print(f"{r:2d} {line}")


# title "功能菜单": band 1  y 26..93, x 183..480
dump("TITLE band (功能菜单?)", 183, 481, 26, 94, cols=20, rows=16)
# band 2 : "倒库" + cursor + something, y 100..175, x 66..659
dump("BAND2 full", 60, 660, 100, 176, cols=40, rows=16)
# band 3 : stray ',', y 187..225
dump("BAND3 stray", 60, 660, 187, 226, cols=40, rows=12)
# band 4 : box + 侧停, y 273..349
dump("BAND4 box+侧停", 40, 660, 273, 350, cols=40, rows=16)
