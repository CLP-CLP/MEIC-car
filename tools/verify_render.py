#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_render.py —— 把重写后的 menu.c 界面渲染成图片，用于肉眼确认
   同时输出 ASCII 点阵，便于逐行核对行间是否干净。"""
import re
from PIL import Image, ImageDraw

exec(open("tools/verify_layout.py", encoding="utf-8").read().split("fail = 0")[0])

MENU = 菜单
SUB = 子菜单


def ascii_dump(tag):
    print(f"--- {tag} ---")
    for y in range(H):
        line = "".join("#" if buf[(y >> 3) * W + x] & (1 << (y & 7)) else "."
                       for x in range(W))
        print(f"{y:2d}|{line}|")
    print()


def to_img(scale=5):
    im = Image.new("RGB", (W, H), (0, 0, 0))
    p = im.load()
    for y in range(H):
        for x in range(W):
            if buf[(y >> 3) * W + x] & (1 << (y & 7)):
                p[x, y] = (255, 255, 255)
    return im.resize((W * scale, H * scale), Image.NEAREST)


shots = []
for sel in range(8):
    draw_screen("功能菜单", MENU, len(MENU), sel, 3)
    shots.append((f"主菜单 sel={sel} ({MENU[sel]})", to_img()))

draw_screen("原始值", SUB, 2, 0, 2)
shots.append(("子菜单 原始值", to_img()))
draw_screen("巡线", SUB, 2, 1, 2)
shots.append(("子菜单 巡线[返回]", to_img()))

# 原始值页
clear()
draw_cn(5, 0, "原始值")
for i in range(5):
    draw_char(0, 2 + i, "c")
    draw_char(1, 2 + i, str(i))
    draw_char(2, 2 + i, ":")
    for k, d in enumerate("1024"):
        draw_char(3 + k, 2 + i, d)
draw_char(0, 7, "A")
shots.append(("原始值页", to_img()))

S = 5
cols = 4
pad, top = 12, 24
rows_n = (len(shots) + cols - 1) // cols
sheet = Image.new("RGB", (cols * (W * S + pad) + pad, rows_n * (H * S + top + pad) + pad),
                  (24, 24, 28))
d = ImageDraw.Draw(sheet)
for i, (label, im) in enumerate(shots):
    cx, cy = i % cols, i // cols
    x = pad + cx * (W * S + pad)
    y = pad + cy * (H * S + top + pad)
    d.text((x, y), label, fill=(255, 210, 80))
    sheet.paste(im, (x, y + top))
sheet.save("tools/render_all.png")
print("wrote tools/render_all.png", sheet.size)

draw_screen("功能菜单", MENU, len(MENU), 0, 3)
ascii_dump("主菜单 sel=0 (巡线)")
