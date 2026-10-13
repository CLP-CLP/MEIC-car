#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
临时验证脚本：在电脑上复刻 OLED_填充灰阶矩形() 的抖动算法，
把主屏幕那五个传感器框渲染成 PNG，用来肉眼确认灰阶是不是连续的。
只依赖 Pillow；不参与固件编译。
"""

from PIL import Image

W, H = 128, 64

# 与 src/oled.c 里的 抖动门槛[8][8] 完全一致（= (Bayer*2+1)*1000/128）
THRESH = [
    [8, 508, 133, 633, 39, 539, 164, 664],
    [758, 258, 883, 383, 789, 289, 914, 414],
    [195, 695, 70, 570, 227, 727, 102, 602],
    [945, 445, 820, 320, 977, 477, 852, 352],
    [55, 555, 180, 680, 23, 523, 148, 648],
    [805, 305, 930, 430, 773, 273, 898, 398],
    [242, 742, 117, 617, 211, 711, 86, 586],
    [992, 492, 867, 367, 961, 461, 836, 336],
]

FB = bytearray(W * H // 8)          # 显存，页格式


def clear():
    for i in range(len(FB)):
        FB[i] = 0


def put(x, y):
    if 0 <= x < W and 0 <= y < H:
        FB[(y >> 3) * W + x] |= 1 << (y & 7)


def rect(x0, y0, x1, y1):
    for x in range(x0, x1 + 1):
        put(x, y0)
        put(x, y1)
    for y in range(y0, y1 + 1):
        put(x0, y)
        put(x1, y)


def gray_rect(x0, y0, x1, y1, level):
    """复刻 OLED_填充灰阶矩形：level 0~1000"""
    level = max(0, min(1000, level))
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if level >= THRESH[y & 7][x & 7]:
                put(x, y)


def gray_per_mille(idx, raw, thr=450, strong=550):
    """复刻 灰度_黑度千分()：默认阈值 450 / 强阈值 550"""
    span = 4 * (strong - thr)
    if span < 80:
        span = 80
    white = thr - span // 2
    d = ((raw - white) * 1000) // span
    return max(0, min(1000, d))


# ---------------- 上半：主屏幕那一帧 ----------------
clear()

RAWS = [250, 350, 450, 550, 650]     # 第 1~5 路的原始值
levels = []
for i in range(5):
    road = 4 - i                     # 和 menu_tree.c 一样：左边显示第 5 路
    raw = RAWS[road]
    lv = gray_per_mille(road, raw)
    levels.append(lv)
    x0 = 6 + i * 24
    gray_rect(x0 + 1, 3, x0 + 18, 14, lv)
    rect(x0, 2, x0 + 19, 15)
    if raw >= 450:                   # 判为压线时套内框
        rect(x0 + 1, 3, x0 + 18, 14)

# ---------------- 下半：0~1000 均匀分 9 档，看区分度 ----------------
for i in range(9):
    lv = i * 125
    x0 = 2 + i * 14
    gray_rect(x0 + 1, 25, x0 + 12, 36, lv)
    rect(x0, 24, x0 + 13, 37)


def bar_rect(x0, y0, x1, y1, level):
    """备选方案：竖直电平柱（从底部长上来），顶部按余数用横向长度细分"""
    level = max(0, min(1000, level))
    h = level * (y1 - y0 + 1) / 1000.0
    hi = int(h)
    frac = h - hi
    for k in range(hi):                       # 下面的整行填满
        for x in range(x0, x1 + 1):
            put(x, y1 - k)
    if frac > 0 and hi <= (y1 - y0):          # 再上一行按比例横向填
        width = int(round(frac * (x1 - x0 + 1)))
        for x in range(x0, x0 + width):
            put(x, y1 - hi)
    # 50%（判黑阈值）位置擦出一条横线，标出"过没过阈值"
    thr_y = y1 - (y1 - y0 + 1) // 2
    for x in range(x0, x1 + 1):
        FB[(thr_y >> 3) * W + x] &= ~(1 << (thr_y & 7))


# ---------------- 最下面：同样的 9 档，换成电平柱 ----------------
for i in range(9):
    lv = i * 125
    x0 = 2 + i * 14
    bar_rect(x0 + 1, 45, x0 + 12, 56, lv)
    rect(x0, 44, x0 + 13, 57)

# ---------------- 导出 ----------------
img = Image.new("1", (W, H), 0)
px = img.load()
for y in range(H):
    for x in range(W):
        if FB[(y >> 3) * W + x] & (1 << (y & 7)):
            px[x, y] = 1

SCALE = 6
big = img.resize((W * SCALE, H * SCALE), Image.NEAREST)
out = "tools/preview_screen.png"
big.save(out)
print("raw -> level:", list(zip(RAWS, [gray_per_mille(0, r) for r in RAWS])))
print("layout levels (5th..1st road):", levels)
print("saved", out)

# 顺带校验：点数比例应该严格等于灰度
for lv in (0, 125, 250, 500, 750, 1000):
    clear()
    gray_rect(0, 0, 15, 15, lv)
    on = sum(bin(b).count("1") for b in FB)
    print("level %4d -> %3d/256 pixels on (%5.1f%%)" % (lv, on, on * 100 / 256))
