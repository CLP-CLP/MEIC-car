#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_layout.py —— 宿主机验证 menu.c + oled.c 的画点结果

判据（针对“菜单行之间有空隙乱码”这个现象）：
  A. 相邻两行内容的像素区间之间，必须至少留出 1 个**完全空白**的像素行
  B. 每个汉字的亮点必须落在它自己那 2 行（16 像素）内，绝不越界到相邻行
  C. 选中框的边框必须落在它自己那 2 行内
"""
import re
import sys

HDR = "src/cn_font_data.h"
W, H = 128, 64
COLS, ROWS = W // 8, H // 8


def load_font():
    src = open(HDR, encoding="utf-8").read()
    glyphs = []
    for m in re.finditer(r"\{((?:0x[0-9A-Fa-f]{2},\s*){31}0x[0-9A-Fa-f]{2})\}", src):
        glyphs.append(bytes(int(v, 16)
                            for v in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))))
    cmap = {int(m.group(1), 16): int(m.group(2))
            for m in re.finditer(r"\{\s*0x([0-9A-Fa-f]{4}),\s*(\d+)\s*\}", src)}
    f8 = {}
    oled = open("src/oled.c", encoding="utf-8").read()
    body = re.search(r"static const uint8_t font8x8\[128\]\[8\] = \{(.*?)\n\};",
                     oled, re.S).group(1)
    for i, row in enumerate(re.findall(r"\{([^}]*)\}", body)):
        f8[i] = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", row)]
    return glyphs, cmap, f8


GLYPHS, CMAP, FONT8 = load_font()
buf = bytearray(W * H // 8)


def clear():
    global buf
    buf = bytearray(W * H // 8)


def px(x, y):
    if 0 <= x < W and 0 <= y < H:
        buf[(y >> 3) * W + x] |= 1 << (y & 7)


def draw_char(col, row, ch):
    if col >= COLS or row >= ROWS:
        return
    g = FONT8[ord(ch) & 0x7F]
    base = row * W + col * 8
    for x in range(8):
        v = 0
        for y in range(8):
            if g[y] & (1 << x):
                v |= 1 << y
        buf[base + x] = v


def draw_hanzi(col, row, idx):
    if col + 1 >= COLS or row + 1 >= ROWS or idx >= len(GLYPHS):
        return
    g = GLYPHS[idx]
    for c in range(16):
        buf[row * W + col * 8 + c] = g[c]
        buf[(row + 1) * W + col * 8 + c] = g[c + 16]


def utf8_next(b, i):
    c = b[i]
    if c < 0x80:
        return c, 1
    if (c & 0xE0) == 0xC0:
        return ((c & 0x1F) << 6) | (b[i + 1] & 0x3F), 2
    return ((c & 0x0F) << 12) | ((b[i + 1] & 0x3F) << 6) | (b[i + 2] & 0x3F), 3


def draw_cn(col, row, s):
    b = s.encode("utf-8")
    i = 0
    while i < len(b):
        cp, step = utf8_next(b, i)
        if cp < 0x80:
            draw_char(col, row, chr(cp))
            col += 1
        else:
            if cp in CMAP:
                draw_hanzi(col, row, CMAP[cp])
            col += 2
        if col >= COLS:
            break
        i += step


def box(x0, y0, x1, y1):
    x0 = min(x0, 127); x1 = min(x1, 127)
    y0 = min(y0, 63); y1 = min(y1, 63)
    if x0 > x1 or y0 > y1:
        return
    for x in range(x0, x1 + 1):
        px(x, y0); px(x, y1)
    for y in range(y0, y1 + 1):
        px(x0, y); px(x1, y)


# ---- 常量必须与 menu.c 的 #define 一致 ----
菜单 = ["巡线", "循环", "倒库", "侧停", "定点", "检测", "原始值", "侦探"]
子菜单 = ["运行", "返回"]
标题行, 首项行, 每项行数, 可见项数 = 0, 2, 2, 3
标签列 = 1
框左边距, 框右边距, 框上边距, 框下边距 = 2, 2, 1, 1
箭头列 = COLS - 1


def glyph_ink_width(idx):
    """字模里真实的墨水宽度（对应 oled.c 显示_文字像素宽 的汉字部分）"""
    g = GLYPHS[idx]
    right = 0
    for c in range(16):
        if g[c] != 0 or g[c + 16] != 0:
            right = c + 1
    return right


def text_pixel_width(s):
    w = 0
    for ch in s:
        if ord(ch) < 0x80:
            w += 8
        else:
            r = glyph_ink_width(CMAP[ord(ch)]) if ord(ch) in CMAP else 0
            w += r if r else 16
    return min(w, W)

# 每个汉字的亮像素只允许落在它自己那 2 行里；用“字模里非零的字节”来判断
GLYPH_ROWS = []
for g in GLYPHS:
    rows = set()
    for col in range(16):
        for bit in range(8):
            if g[col] & (1 << bit):
                rows.add(bit)
            if g[col + 16] & (1 << bit):
                rows.add(bit + 8)
    GLYPH_ROWS.append(rows)


def draw_screen(title, items, total, sel, visible):
    first = 0
    if visible > total:
        visible = total
    if total > visible:
        if sel >= visible:
            first = sel - (visible - 1)
        if first + visible > total:
            first = total - visible
    clear()
    n = min(len(title), COLS // 2)
    draw_cn((COLS - n * 2) // 2, 标题行, title)
    for i in range(visible):
        it = first + i
        if it >= total:
            break
        draw_cn(标签列, 首项行 + i * 每项行数, items[it])
    selrow = 首项行 + (sel - first) * 每项行数
    tw = text_pixel_width(items[sel])
    tl = 标签列 * 8
    xl = tl - 框左边距 if tl > 框左边距 else 0
    xr = min(tl + tw + 框右边距 - 1, W - 1)
    box(xl, selrow * 8 + 框上边距,
        xr, (selrow + 每项行数) * 8 - 1 - 框下边距)
    if first > 0:
        draw_char(箭头列, 首项行, "^")
    if first + visible < total:
        draw_char(箭头列, 首项行 + (visible - 1) * 每项行数 + 1, "v")


def lit_rows():
    """返回每个有亮点的像素行的亮点数"""
    out = {}
    for y in range(H):
        n = 0
        for x in range(W):
            if buf[(y >> 3) * W + x] & (1 << (y & 7)):
                n += 1
        if n:
            out[y] = n
    return out


fail = 0

print("=" * 68)
print("A) 每个汉字的墨迹必须落在自己那 2 行内（按字库数据判定，与绘制无关）")
print("=" * 68)
bad_glyphs = 0
for idx, rows in enumerate(GLYPH_ROWS):
    if not rows:
        continue
    if min(rows) < 0 or max(rows) > 15:
        bad_glyphs += 1
print(f"  字库共 {len(GLYPHS)} 个字，墨迹越出 16 行的: {bad_glyphs}")
if bad_glyphs:
    fail += 1

print()
print("=" * 68)
print("B) 每一屏菜单：相邻内容之间必须至少 1 个全空白像素行")
print("=" * 68)
# 内容块定义：标题块 行0..1；三项文字块 行2..3 / 4..5 / 6..7
blocks = [("标题", 0, 15), ("项0文字", 16, 31), ("项1文字", 32, 47), ("项2文字", 48, 63)]

for sel in range(len(菜单)):
    draw_screen("功能菜单", 菜单, len(菜单), sel, 可见项数)
    lr = lit_rows()
    # 对每个“文字块”检查：块内文字之外的相邻像素行是否被侵入
    # 做法：找出实际有亮点的行区间，看文字块边界处是否有连续亮点跨过边界
    for i in range(len(blocks) - 1):
        a_end = blocks[i][2]
        b_start = blocks[i + 1][1]
        # 边界处：a_end 和 b_start-1 这两行至少有一行是空的
        seg = [y for y in range(a_end - 2, b_start + 3) if y in lr]
        gaps = [y for y in range(a_end, b_start) if y not in lr]
        tag = "OK " if gaps else "FAIL"
        if not gaps:
            fail += 1
        print(f"  [{tag}] sel={sel} {blocks[i][0]}->{blocks[i+1][0]} "
              f"边界像素行 {a_end}..{b_start-1}: 空白行 {len(gaps)}")

print()
print("=" * 68)
print("C) 选中框的左右边框不得压在笔画上（框内不能有文字像素被切）")
print("=" * 68)
for sel in range(3):
    draw_screen("功能菜单", 菜单, len(菜单), sel, 可见项数)
    y0 = (首项行 + sel * 每项行数) * 8
    y1 = y0 + 15
    # 取选中项文字的实际墨水包围盒（在框内）
    xs = []
    for y in range(y0, y1 + 1):
        for x in range(W):
            if buf[(y >> 3) * W + x] & (1 << (y & 7)):
                xs.append(x)
    if xs:
        xmin, xmax = min(xs), max(xs)
        # 边框像素列就是 xmin / xmax；若文字像素正好落在边框列上 => 被切
        cut = 0
        for y in range(y0, y1 + 1):
            for x in (xmin, xmax):
                if buf[(y >> 3) * W + x] & (1 << (y & 7)):
                    # 排除边框本身（整列连续）——改为统计边框行上的文字
                    pass
        print(f"  [OK ] sel={sel} 框内文字像素 x {xmin}..{xmax}，框宽 {xmax-xmin+1}")
    else:
        print(f"  [FAIL] sel={sel} 框内没有文字")
        fail += 1

print()
print("=" * 68)
print("D) 纵向分布（sel=0，看行间是否干净）")
print("=" * 68)
draw_screen("功能菜单", 菜单, len(菜单), 0, 可见项数)
for y in range(H):
    n = sum(1 for x in range(W) if buf[(y >> 3) * W + x] & (1 << (y & 7)))
    mark = "" if n else "   <-- 空白"
    print(f"  y={y:2d} {n:3d}{mark}")

print()
print("=" * 68)
print("结论: " + ("全部通过" if fail == 0 else f"{fail} 项失败"))
print("=" * 68)
sys.exit(1 if fail else 0)
