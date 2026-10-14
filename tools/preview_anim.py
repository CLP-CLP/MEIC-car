#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
临时验证脚本：在电脑上复刻菜单页 + OLED_合成滑动() + OLED_显示中文串滚动()，
把"进子菜单 / 返回"的滑动方向和跑马灯的滚动效果画成 PNG，用来肉眼确认。

只依赖 Pillow；不参与固件编译。
"""

from PIL import Image, ImageDraw, ImageFont

W, H = 128, 64
FONT_PATH = "C:/Windows/Fonts/msyh.ttc"
_font_cache = {}


def font(size=15):
    if size not in _font_cache:
        _font_cache[size] = ImageFont.truetype(FONT_PATH, size)
    return _font_cache[size]


# ---------------------------------------------------------------- 显存模拟
def new_fb():
    return [[0] * W for _ in range(H)]


def put(fb, x, y, c=1):
    if 0 <= x < W and 0 <= y < H:
        fb[y][x] = c


def fill(fb, x0, y0, x1, y1, c=1):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            put(fb, x, y, c)


def frame(fb, x0, y0, x1, y1, c=1):
    for x in range(x0, x1 + 1):
        put(fb, x, y0, c)
        put(fb, x, y1, c)
    for y in range(y0, y1 + 1):
        put(fb, x0, y, c)
        put(fb, x1, y, c)


def glyph(ch, h=16):
    """把一个字符渲染成 h 像素高的二值点阵（返回宽 x 高的二维表）"""
    w = 8 if ord(ch) < 0x80 else 16
    img = Image.new("L", (w * 2, h * 2), 0)
    d = ImageDraw.Draw(img)
    d.text((w // 2, 0), ch, font=font(15 if w == 16 else 11), fill=255)
    img = img.crop((0, 0, w, h))
    return [[1 if img.getpixel((x, y)) > 100 else 0 for x in range(w)]
            for y in range(h)]


def draw_text(fb, x, y, s, c=1, clip=None):
    """和 oled.c 一样：汉字 16 宽占满行，ASCII 8 宽且下移 4 像素"""
    cx = x
    for ch in s:
        g = glyph(ch)
        gw = len(g[0])
        off = 4 if gw == 8 else 0          # ASCII 在 16 像素行里居中
        for gy in range(len(g)):
            for gx in range(gw):
                if g[gy][gx] == 0:
                    continue
                px, py = cx + gx, y + off + gy
                if clip is not None and not (clip[0] <= px <= clip[2]):
                    continue
                put(fb, px, py, c)
        cx += gw
    return cx


# ---------------------------------------------------------------- 合成滑动
def compose(左图, 右图, d):
    """复刻 OLED_合成滑动()：d 越大内容越往左走"""
    out = new_fb()
    for y in range(H):
        for x in range(W):
            src = x + d
            out[y][x] = 左图[y][src] if src < W else 右图[y][src - W]
    return out


# ---------------------------------------------------------------- 菜单页
def menu_page(title, items, sel):
    fb = new_fb()
    fill(fb, 0, 0, W - 1, 15, 1)                     # 标题栏反白
    draw_text(fb, 2, 0, title, c=0)
    for i, it in enumerate(items):
        y = 16 + i * 16
        if i == sel:
            frame(fb, 0, y, W - 1, y + 15)
        draw_text(fb, 2, y, it, c=1)
    return fb


调试页 = menu_page("调试", ["巡线传感器读数", "手动校准", "自动校准"], 0)
任务页 = menu_page("任务", ["巡线", "循环", "倒车入库"], 0)

# ---------------------------------------------------------------- 拼图
SCALE = 2
CELL_W, CELL_H = W * SCALE, H * SCALE


def to_img(fb):
    img = Image.new("1", (W, H), 0)
    px = img.load()
    for y in range(H):
        for x in range(W):
            if fb[y][x]:
                px[x, y] = 1
    return img.resize((CELL_W, CELL_H), Image.NEAREST)


def strip(frames):
    out = Image.new("1", (CELL_W * len(frames), CELL_H), 0)
    for i, fb in enumerate(frames):
        out.paste(to_img(fb), (i * CELL_W, 0))
    return out


# 进子菜单：偏移 0 -> 128（内容左移），旧页=调试，新页=任务
进 = [compose(调试页, 任务页, d) for d in (0, 48, 96, 128)]
# 返回：偏移 128 -> 0（内容右移），新页=调试，旧页=任务
退 = [compose(调试页, 任务页, d) for d in (128, 80, 32, 0)]

slide = Image.new("1", (CELL_W * 4, CELL_H * 2 + 8), 0)
slide.paste(strip(进), (0, 0))
slide.paste(strip(退), (0, CELL_H + 8))
slide.save("tools/preview_slide.png")
print("saved tools/preview_slide.png")

# ---------------------------------------------------------------- 跑马灯
长标题 = "直接设置四路阈值"
窗左, 窗右 = 2, 116
marquee = []
for 相位 in (0, 36, 72, 108, 144):
    fb = menu_page("高级设置", ["设置最低PWM", "直接设置四路阈值", "返回"], 1)
    # 把选中行擦干净，用滚动的方式重画（复刻 OLED_显示中文串滚动 + 裁剪窗口）
    y = 16 + 16
    fill(fb, 窗左, y, 窗右, y + 15, 0)
    frame(fb, 0, y, W - 1, y + 15)
    周期 = len(长标题) * 16 + 48
    ph = 相位 % 周期
    draw_text(fb, 窗左 - ph, y, 长标题, c=1, clip=(窗左, y, 窗右, y + 15))
    if ph > 0:
        draw_text(fb, 窗左 - ph + 周期, y, 长标题, c=1, clip=(窗左, y, 窗右, y + 15))
    marquee.append(fb)

strip(marquee).save("tools/preview_marquee.png")
print("saved tools/preview_marquee.png")
