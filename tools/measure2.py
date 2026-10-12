#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Measure the new (straight-on) photo: locate the OLED glass, then the content
bands, and derive the real row pitch and glyph height in OLED rows."""
import numpy as np
from PIL import Image

im = Image.open("IMG_20261012_232417..jpg").convert("L")
a = np.asarray(im).astype(float)
H, W = a.shape
print("photo", W, "x", H)

# The glass = dark rectangle with lit text. Find it as the region between the
# bright bezel edges: scan for the big dark area.
dark = a < 60
# row/col support of darkness over long runs
rowsup = dark.mean(axis=1)
colsup = dark.mean(axis=0)
ys = np.where(rowsup > 0.5)[0]
xs = np.where(colsup > 0.5)[0]
print("dark rows", ys.min(), ys.max(), "dark cols", xs.min(), xs.max())

Y0, Y1, X0, X1 = ys.min(), ys.max(), xs.min(), xs.max()
sub = a[Y0:Y1 + 1, X0:X1 + 1]
print("glass crop", sub.shape[1], "x", sub.shape[0])

mask = sub > 140
rp = mask.sum(axis=1)

print("\ncontent bands inside glass (photo-y-offset, height, x extent):")
bands = []
run = None
for i, n in enumerate(rp):
    on = n > 3
    if on and run is None:
        run = i
    elif not on and run is not None:
        bands.append((run, i - 1))
        run = None
if run is not None:
    bands.append((run, len(rp) - 1))

for k, (t, b) in enumerate(bands):
    if b - t < 2:
        continue
    cols = np.where(mask[t:b + 1].any(axis=0))[0]
    print(f"  band {k:2d}: y {t:4d}..{b:4d} h={b-t+1:4d}  x {cols.min():4d}..{cols.max():4d}"
          f"  maxlit={int(rp[t:b+1].max())}")

print("\ncolumn profile (glass-x : lit count) to find the selection box extent:")
cp = mask.sum(axis=0)
print("first/last lit column:", np.where(cp > 0)[0].min(), np.where(cp > 0)[0].max())

# long horizontal lines = box borders
print("\nrows with wide horizontal lines (>150 lit px):")
for i, n in enumerate(rp):
    if n > 150:
        xs2 = np.where(mask[i])[0]
        print(f"  glass-y {i:4d} lit={int(n):4d} x {xs2.min():4d}..{xs2.max():4d}")
