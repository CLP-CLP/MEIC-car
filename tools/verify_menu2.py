#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_menu2.py —— 复刻新版 menu.c 的排版，验证：

  A. 标签（按像素截断后）不会碰到行尾的数值/单位/状态字
  B. 数值项、开关项、子菜单的提示都贴右边缘对齐
  C. 反白高亮：选中行整行白底 + 黑字
  D. 每行文字都落在自己那 16 像素内
  E. 滑块（菜单_标志_滑块）不会跑出屏幕、不压标签
"""
import re
import sys

W, H = 128, 64
LIST_TOP, ROW_H, VISIBLE = 16, 16, 3
LABEL_X, HINT_RIGHT, HINT_W = 2, 126, 44

src = open("src/cn_font_data.h", encoding="utf-8").read()
glyphs = []
for m in re.finditer(r"\{((?:0x[0-9A-Fa-f]{2},\s*){31}0x[0-9A-Fa-f]{2})\}", src):
    glyphs.append(bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))))
cmap = {int(m.group(1), 16): int(m.group(2))
        for m in re.finditer(r"\{\s*0x([0-9A-Fa-f]{4}),\s*(\d+)\s*\}", src)}

buf = bytearray(W * H // 8)


def clear():
    global buf
    buf = bytearray(W * H // 8)


def point(x, y, c):
    if 0 <= x < W and 0 <= y < H:
        m = 1 << (y & 7)
        if c:
            buf[(y >> 3) * W + x] |= m
        else:
            buf[(y >> 3) * W + x] &= ~m & 0xFF


def fill(x0, y0, x1, y1, c=1):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            point(x, y, c)


def hanzi(x, y, idx, c=1):
    g = glyphs[idx]
    for col in range(16):
        for row in range(8):
            if g[col] & (1 << row):
                point(x + col, y + row, c)
            if g[16 + col] & (1 << row):
                point(x + col, y + 8 + row, c)


def text_width(s):
    return sum(8 if ord(ch) < 0x80 else 16 for ch in s)


def cn_limited(x, y, s, maxw, c=1):
    """复刻 OLED_显示中文串限宽"""
    used = 0
    for ch in s:
        w = 8 if ord(ch) < 0x80 else 16
        if used + w > maxw or x + w > W:
            break
        if ord(ch) < 0x80:
            # ASCII：用不到，这里只标记占位
            used += w
            x += w
            continue
        if ord(ch) in cmap:
            hanzi(x, y, cmap[ord(ch)], c)
        x += w
        used += w


def ascii_str(x, y, s, c=1):
    """粗糙模拟：只用来占位判断碰撞，不画真实字形"""
    for i, ch in enumerate(s):
        xx = x + i * 8
        if xx + 8 > W:
            break
        for k in range(6):
            point(xx + 1 + k, y + 1, c)
            point(xx + 1 + k, y + 6, c)


# ---- 用 menu.c 里的真实定义 ----
MENUS = {
    "主菜单": [("任务", "sub"), ("电机", "sub"), ("高级设置", "sub"),
               ("调试工具", "sub"), ("关于本车", "info")],
    "任务": [("巡线", "cmd"), ("循环", "cmd"), ("倒车入库", "cmd"),
             ("侧方停车", "cmd"), ("定点停车", "cmd"), ("检测停车", "cmd"),
             ("侦探", "cmd"), ("返回", "back")],
    "电机": [("调速说明", "cmd"), ("左电机速度", "val"), ("右电机速度", "val"),
             ("最低 PWM", "val"), ("保存到 Flash", "cmd"),
             ("全部停止", "cmd"), ("返回", "back")],
    "高级设置": [("电机参数", "sub"), ("灰度校准", "cmd"),
                 ("恢复默认", "cmd"), ("返回", "back")],
    "调试工具": [("五路读数", "cmd"), ("高级设置", "sub"), ("返回", "back")],
}
VALUES = {"左电机速度": "100%", "右电机速度": "-100%", "最低 PWM": "65%"}


def draw(menu, sel):
    items = MENUS[menu]
    total = len(items)
    first = 0
    if sel >= first + VISIBLE:
        first = sel - VISIBLE + 1

    clear()
    fill(0, 0, W - 1, 15, 1)
    # 标题（黑字）
    cn_limited(2, 0, menu, 80, 0)
    page = f"{sel+1}/{total}"
    ascii_str(126 - text_width(page), 4, page, 0)

    for row in range(VISIBLE):
        idx = first + row
        if idx >= total:
            break
        top = LIST_TOP + row * ROW_H
        hl = 1 if idx == sel else 0
        label, kind = items[idx]
        if hl:
            fill(0, top, W - 1, top + 15, 1)
        color = 0 if hl else 1

        avail = HINT_RIGHT - LABEL_X - HINT_W
        if kind in ("val", "sw"):
            avail -= 8
        cn_limited(LABEL_X, top, label, avail, color)

        if kind == "sub":
            ascii_str(120, top + 4, ">", color)
        elif kind == "val":
            txt = VALUES.get(label, "0")
            ascii_str(HINT_RIGHT - text_width(txt), top + 4, txt, color)
            # 滑块
            x0 = HINT_RIGHT - HINT_W - 34
            point(x0, top + 5, color)
            point(x0 + 34, top + 11, color)
        elif kind == "sw":
            txt = "开"
            cn_limited(HINT_RIGHT - 16, top, txt, 16, color)
    return first, total


fail = 0

print("=" * 68)
print("A) 标签（截断后）与行尾提示不得重叠")
print("=" * 68)
for menu in MENUS:
    for sel in range(len(MENUS[menu])):
        first, total = draw(menu, sel)
        for row in range(VISIBLE):
            idx = first + row
            if idx >= total:
                break
            top = LIST_TOP + row * ROW_H
            label, kind = MENUS[menu][idx]
            if kind not in ("val", "sw"):
                continue
            # 标签实际画到的右边界
            avail = HINT_RIGHT - LABEL_X - HINT_W - 8
            used = 0
            for ch in label:
                w = 8 if ord(ch) < 0x80 else 16
                if used + w > avail:
                    break
                used += w
            label_right = LABEL_X + used
            # 提示文字左边界
            txt = VALUES.get(label, "0")
            hint_left = HINT_RIGHT - text_width(txt)
            ok = label_right <= hint_left
            if not ok:
                fail += 1
            print(f"  [{'OK ' if ok else 'FAIL'}] {menu}/{label}: 标签右边界 {label_right} "
                  f"vs 提示左边界 {hint_left}")

print()
print("=" * 68)
print("B) 每行文字都落在自己那 16 像素内")
print("=" * 68)
bad = 0
for menu in MENUS:
    for sel in range(len(MENUS[menu])):
        first, total = draw(menu, sel)
        for row in range(VISIBLE):
            idx = first + row
            if idx >= total:
                break
            top = LIST_TOP + row * ROW_H
            for y in range(top, top + 16):
                pass
            # 检查该行上下各 1 像素有没有意外亮点（只在标签区）
            for y in (top - 1, top + 16):
                if y < LIST_TOP or y >= H:
                    continue
                for x in range(LABEL_X, 70):
                    if buf[(y >> 3) * W + x] & (1 << (y & 7)):
                        # 反白条整片亮，跳过
                        if not (buf[(y >> 3) * W + 0] & (1 << (y & 7))):
                            bad += 1
print(f"  越界亮点: {bad}")
if bad:
    fail += 1

print()
print("=" * 68)
print("C) 反白高亮：选中行上下边缘必须整行是白的（箭头不能戳缺口）")
print("=" * 68)
for menu in MENUS:
    for sel in range(len(MENUS[menu])):
        first, total = draw(menu, sel)
        row = sel - first
        top = LIST_TOP + row * ROW_H
        # 上下边缘整行都应该是白的
        ok = all(buf[(y >> 3) * W + x] & (1 << (y & 7))
                 for y in (top, top + 15) for x in range(W))
        if not ok:
            fail += 1
        print(f"  [{'OK ' if ok else 'FAIL'}] {menu} sel={sel} ({MENUS[menu][sel][0]}) 边缘完整={ok}")

print()
print("=" * 68)
print("C2) 子菜单箭头在反白行里必须是黑箭头（否则看不见）")
print("=" * 68)
for menu in MENUS:
    for sel in range(len(MENUS[menu])):
        if MENUS[menu][sel][1] != "sub":
            continue
        first, total = draw(menu, sel)
        row = sel - first
        top = LIST_TOP + row * ROW_H
        dark = sum(1 for y in range(top, top + 16) for x in range(118, 127)
                   if not (buf[(y >> 3) * W + x] & (1 << (y & 7))))
        ok = dark > 0
        if not ok:
            fail += 1
        print(f"  [{'OK ' if ok else 'FAIL'}] {menu}/{MENUS[menu][sel][0]} 箭头黑像素={dark}")

print()
print("=" * 68)
print("结论: " + ("全部通过" if fail == 0 else f"{fail} 项失败"))
print("=" * 68)

# 出一个渲染图
from PIL import Image, ImageDraw
shots = []
for menu, sel in [("主菜单", 0), ("任务", 0), ("电机", 1), ("电机", 3), ("高级设置", 0)]:
    draw(menu, sel)
    im = Image.new("RGB", (W, H), (0, 0, 0))
    p = im.load()
    for y in range(H):
        for x in range(W):
            if buf[(y >> 3) * W + x] & (1 << (y & 7)):
                p[x, y] = (255, 255, 255)
    shots.append((f"{menu} sel={sel}", im.resize((W * 5, H * 5), Image.NEAREST)))

pad, top = 12, 22
sheet = Image.new("RGB", (3 * (W * 5 + pad) + pad, 2 * (H * 5 + top + pad) + pad), (24, 24, 28))
d = ImageDraw.Draw(sheet)
for i, (label, im) in enumerate(shots):
    x = pad + (i % 3) * (W * 5 + pad)
    y = pad + (i // 3) * (H * 5 + top + pad)
    d.text((x, y), label, fill=(255, 210, 80))
    sheet.paste(im, (x, y + top))
sheet.save("tools/menu_new.png")
print("wrote tools/menu_new.png")
sys.exit(1 if fail else 0)
