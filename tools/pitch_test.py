#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Render the menu at several vertical pitches with the repo font, to pick a
layout where 13px-tall glyphs never get clipped by the 16px row pitch."""
import re
from PIL import Image, ImageDraw

src = open("src/cn_font_data.h", encoding="utf-8").read()
glyphs = []
for m in re.finditer(r"\{((?:0x[0-9A-Fa-f]{2},\s*){31}0x[0-9A-Fa-f]{2})\}", src):
    glyphs.append(bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))))
cmap = {int(m.group(1), 16): int(m.group(2))
        for m in re.finditer(r"\{\s*0x([0-9A-Fa-f]{4}),\s*(\d+)\s*\}", src)}

FONT8 = {}
oled = open("src/oled.c", encoding="utf-8").read()
body = re.search(r"static const uint8_t font8x8\[128\]\[8\] = \{(.*?)\n\};", oled, re.S).group(1)
for i, row in enumerate(re.findall(r"\{([^}]*)\}", body)):
    FONT8[i] = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", row)]

W, H = 128, 64
MENU = ["巡线", "循环", "倒库", "侧停", "定点", "检测", "原始值", "侦探"]


class FB:
    def __init__(self):
        self.b = bytearray(W * H // 8)

    def char(self, col, row, ch):
        if col >= 16 or row >= 8:
            return
        g = FONT8[ord(ch) & 0x7F]
        base = row * W + col * 8
        for x in range(8):
            v = 0
            for y in range(8):
                if g[y] & (1 << x):
                    v |= 1 << y
            self.b[base + x] = v

    def cn(self, col, row, s):
        for ch in s:
            idx = cmap.get(ord(ch))
            if idx is not None and (col + 1) < 16 and (row + 1) < 8:
                g = glyphs[idx]
                for c in range(16):
                    self.b[row * W + col * 8 + c] = g[c]
                    self.b[(row + 1) * W + col * 8 + c] = g[c + 16]
            col += 2

    def box(self, x0, y0, x1, y1):
        for x in range(x0, x1 + 1):
            self.b[(y0 >> 3) * W + x] |= 1 << (y0 & 7)
            self.b[(y1 >> 3) * W + x] |= 1 << (y1 & 7)
        for y in range(y0, y1 + 1):
            self.b[(y >> 3) * W + x0] |= 1 << (y & 7)
            self.b[(y >> 3) * W + x1] |= 1 << (y & 7)

    def img(self, scale=5):
        im = Image.new("RGB", (W, H), (0, 0, 0))
        p = im.load()
        for y in range(H):
            for x in range(W):
                if self.b[(y >> 3) * W + x] & (1 << (y & 7)):
                    p[x, y] = (255, 255, 255)
        return im.resize((W * scale, H * scale), Image.NEAREST)


def layout(pitch_rows, first_row, sel, box_inset):
    """pitch_rows = rows per menu item (grid rows)."""
    fb = FB()
    fb.cn((16 - 8) // 2, 0, "功能菜单")
    for i in range(3):
        fb.cn(1, first_row + i * pitch_rows, MENU[i])
    y0 = (first_row + sel * pitch_rows) * 8 + box_inset
    y1 = (first_row + (sel + 1) * pitch_rows) * 8 - box_inset - 1
    fb.box(4, y0, 123, y1)
    fb.char(15, first_row + 2 * pitch_rows + 1, "v")
    return fb


variants = [
    ("A  当前代码: 每项 2 行(16px), 框 12px", layout(2, 2, 0, 2)),
    ("B  每项 2 行, 框放宽到 14px", layout(2, 2, 0, 1)),
    ("C  每项 3 行(24px), 框 22px  (一屏 2 项)", layout(3, 2, 0, 1)),
]

S = 5
pad, top = 14, 26
sheet = Image.new("RGB", (W * S + pad * 2, (H * S + top + pad) * len(variants)), (24, 24, 28))
d = ImageDraw.Draw(sheet)
for i, (label, fb) in enumerate(variants):
    y = i * (H * S + top + pad)
    d.text((pad, y + 8), label, fill=(255, 210, 80))
    sheet.paste(fb.img(S), (pad, y + top))
sheet.save("tools/pitch_variants.png")
print("wrote tools/pitch_variants.png")
