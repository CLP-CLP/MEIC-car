#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""挑选字号/字体：找出“最大墨迹能装进 14x14 以内、且不需要缩放”的组合。

缩放会糊笔画（13px 时“菜单”就是被缩糊的），所以优先选天生就小的字号。
"""
import re
import os
from PIL import Image, ImageDraw, ImageFont

SRC = "src\\cn_font_data.h"
txt = open(SRC, encoding="utf-8").read()
chars = [c for c in sorted(set(re.findall(r"U\+[0-9A-Fa-f]{4} '(.)'", txt))) if ord(c) > 0x7F]
if not chars:
    chars = [c for c in sorted(set(re.findall(r"\{\s*0x[0-9A-Fa-f]{4},\s*\d+\s*\},\s*/\*\s*'(.)'", txt)))
             if ord(c) > 0x7F]
print(f"测试字符数: {len(chars)}")

FONTS = [
    "C:/Windows/Fonts/simsun.ttc",
    "C:/Windows/Fonts/simhei.ttf",
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/Deng.ttf",
]

print(f"\n{'字体':22s} {'pt':>3s} {'最大墨迹':>8s} {'需缩放':>6s}")
best = None
for fp in FONTS:
    if not os.path.exists(fp):
        continue
    for size in range(11, 17):
        font = ImageFont.truetype(fp, size)
        mw = mh = 0
        for ch in chars:
            img = Image.new("L", (48, 48), 0)
            ImageDraw.Draw(img).text((4, 4), ch, font=font, fill=255)
            bb = img.getbbox()
            if not bb:
                continue
            w, h = bb[2] - bb[0], bb[3] - bb[1]
            mw, mh = max(mw, w), max(mh, h)
        need_scale = (mw > 14 or mh > 14)
        tag = "要" if need_scale else "不用"
        print(f"{os.path.basename(fp):22s} {size:3d} {mw:4d}x{mh:<3d} {tag:>6s}")
        if not need_scale and (best is None or max(mw, mh) > best[2]):
            best = (fp, size, max(mw, mh))
    print()

if best:
    print(f"推荐: {os.path.basename(best[0])} size={best[1]}  最大墨迹 {best[2]}px（无需缩放）")
