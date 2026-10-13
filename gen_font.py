#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
===============================================================================
 gen_font.py —— 扫描源码里的中文，生成 16x16 点阵字库头文件

 用法：
     python gen_font.py            # 生成 src/cn_font_data.h
     python gen_font.py --check    # 只自检，不写文件

 编译前由 extra_script.py 自动调用。

 设计要点（这一版重写的核心）
 ---------------------------------------------------------------------------
 1. **每个字的墨迹硬限制在 15x15 以内，并居中放进 16x16 格子。**
    这是修“菜单行与行之间出现被裁掉的笔画”的关键：
    旧版允许字模长到 16 像素高、偏移算成 0，于是 16/17 像素高的字会溢出
    到下一行（行距只有 16 像素），屏上就出现半截笔画、鬼影。
    现在留出上下左右各至少半个像素的余量，**物理上不可能越界**。

 2. 字号自动挑选：从大往小试，取“所有字都能装进 15x15”的那个最大字号，
    尽量把格子填满、笔画饱满（13 号偏细，字距显得空）。

 3. 生成后自带断言自检，任何字越界都会直接报错退出，而不是悄悄产出坏字库。

 4. 位序与 SSD1306 水平寻址一致：
        data[0..15]  = 上半页（行 0..7 ），每字节 bit0 = 该列最上面的像素
        data[16..31] = 下半页（行 8..15）
    oled.c 直接把这 32 字节 memcpy 进帧缓冲即可，无需转置。

 依赖： pip install pillow
===============================================================================
"""

import os
import re
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("缺少 Pillow，请先安装： pip install pillow")
    sys.exit(1)

# ============================ 配置 ===========================================

SRC_DIRS = ["src", "include"]        # 扫描哪些目录里的字符串字面量
OUTPUT_FILE = "src/cn_font_data.h"   # 输出
FONT_PATH = None                     # None = 自动查找；也可写死一个 .ttf/.ttc

CELL = 16                            # 单字格子尺寸（像素）
INK_MAX = 14                         # 墨迹最大尺寸：上下左右各留 1 像素余量

# 自动挑选字号时的候选（从大到小）
FONT_SIZE_CANDIDATES = [16, 15, 14, 13, 12]

# 备选字体（Windows 优先；实测小字号下雅黑/黑体笔画更实，反白高亮时最清楚）
_WIN_FONTS = [
    "C:/Windows/Fonts/msyh.ttc",         # 微软雅黑
    "C:/Windows/Fonts/simhei.ttf",       # 黑体
    "C:/Windows/Fonts/simsun.ttc",       # 宋体
    "C:/Windows/Fonts/Deng.ttf",         # 等线
    "C:/Windows/Fonts/simsunb.ttf",
]
_LINUX_FONTS = [
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
]


def find_chinese_fonts():
    """返回所有可用的候选中文字体（按优先级），供 choose_font 逐个试"""
    import platform

    cands = _WIN_FONTS if platform.system() == "Windows" else _LINUX_FONTS
    return [f for f in cands if os.path.exists(f)]


# ============================ 字符扫描 =======================================

def find_chinese_chars(project_root, dirs):
    """
    扫描 .c/.h 里**字符串字面量**中的中文/全角标点，返回排序后的唯一字符列表。
    注释里的中文不算，代码中的中文标识符也不算。
    """
    chars = set()
    cn = re.compile(r'[\u2190-\u21ff\u3000-\u303f\u4e00-\u9fff\uff00-\uffef]')
    lit = re.compile(r'"((?:[^"\\]|\\.)*)"')

    for d in dirs:
        full = os.path.join(project_root, d)
        if not os.path.isdir(full):
            continue
        for root, subdirs, files in os.walk(full):
            # 跳过嵌套的独立工程（比如 meic/ 里也有一份 main.c）：
            # 只扫本工程自己的代码，否则字库会被别处的文案撑大。
            subdirs[:] = [s for s in subdirs
                          if not os.path.exists(os.path.join(root, s, "platformio.ini"))]
            for fn in files:
                if not fn.endswith((".c", ".h")):
                    continue
                path = os.path.join(root, fn)
                try:
                    with open(path, "r", encoding="utf-8") as fp:
                        content = fp.read()
                except Exception as e:
                    print(f"  警告: 读取 {path} 失败: {e}")
                    continue

                found = set()
                for m in lit.finditer(content):
                    found.update(cn.findall(m.group(1)))
                if found:
                    print(f"  {os.path.relpath(path, project_root)}: {len(found)} 个显示用字符")
                chars.update(found)

    return sorted(chars)


# ============================ 字形渲染 =======================================

def render_ink(ch, font):
    """
    把字符渲染到一张大画布上，返回 (灰度图, 包围盒, 基线相关偏移)。

    只裁左右和上方空白是危险的：**不能按每个字各自的墨迹高度去居中**，
    否则同一行里不同字会上下浮动（比如“巡线”和“倒库”的墨迹高度不一样）。
    所以这里额外返回基线信息，由 glyph_from_ink() 统一对齐基线。
    """
    pad = 12
    canvas = Image.new("L", (CELL * 2 + pad * 2, CELL * 2 + pad * 2), 0)
    ImageDraw.Draw(canvas).text((pad, pad), ch, font=font, fill=255)
    bbox = canvas.getbbox()
    if bbox is None:
        return None, None, None
    # 该字符的排版基线（相对画布顶部）
    try:
        ascent, descent = font.getmetrics()
    except Exception:
        ascent, descent = bbox[3] - bbox[1], 0
    return canvas.crop(bbox), bbox, (ascent, descent, pad)


def glyph_from_ink(ink, meta=None, allow_scale=False):
    """
    把墨迹放进 INK_MAX x INK_MAX 的“安全盒”，再居中放进 16x16，转成 32 字节列序点阵。

    allow_scale=False（默认）时**不做任何缩放**：装不下就返回失败，
    交给 choose_font() 换更小的字号。缩放会糊笔画，宁可换字号也不缩。

    meta = (ascent, descent, pad)：用于把不同字的**基线对齐**，
    避免同一行里的汉字上下浮动。
    """
    iw, ih = ink.size
    if iw <= 0 or ih <= 0:
        return None, None

    if iw > INK_MAX or ih > INK_MAX:
        if not allow_scale:
            return None, (iw, ih)
        scale = min(INK_MAX / iw, INK_MAX / ih)
        nw = max(1, int(round(iw * scale)))
        nh = max(1, int(round(ih * scale)))
        ink = ink.resize((nw, nh), Image.LANCZOS)
        iw, ih = nw, nh

    box = Image.new("L", (INK_MAX, INK_MAX), 0)
    if meta is not None:
        # 所有字共用一条基线：把基线放在安全盒内固定高度，字就不会上下浮动
        ascent = meta[0]
        target_baseline = INK_MAX - 2          # 基线离盒底 2 像素
        y = target_baseline - ascent
        if y + ih > INK_MAX:                   # 太高就往下压
            y = INK_MAX - ih
        if y < 0:                              # 太矮就顶到 0
            y = 0
    else:
        y = (INK_MAX - ih) // 2

    box.paste(ink, ((INK_MAX - iw) // 2, y))

    cell = Image.new("L", (CELL, CELL), 0)
    off = (CELL - INK_MAX) // 2
    cell.paste(box, (off, off))

    # 二值化 -> SSD1306 列序
    px = cell.load()
    data = bytearray(32)
    for col in range(CELL):
        top = 0
        bot = 0
        for row in range(8):
            if px[col, row] > 128:
                top |= 1 << row
            if px[col, row + 8] > 128:
                bot |= 1 << row
        data[col] = top
        data[col + CELL] = bot
    return bytes(data), (iw, ih)


def ink_extent(data):
    """从 32 字节点阵反算墨迹范围，用于自检。返回 (x0,x1,y0,y1) 或 None。"""
    xs = []
    ys = []
    for col in range(CELL):
        top = data[col]
        bot = data[col + CELL]
        for bit in range(8):
            if top & (1 << bit):
                xs.append(col)
                ys.append(bit)
            if bot & (1 << bit):
                xs.append(col)
                ys.append(bit + 8)
    if not xs:
        return None
    return min(xs), max(xs), min(ys), max(ys)


def choose_font(font_paths, chars):
    """
    挑选 (字体, 字号)：优先“**不需要缩放**就能装进 INK_MAX 格子”的组合，
    并且在这种组合里挑墨迹最大的（字最大最饱满）。

    缩放会糊笔画，所以只有实在找不到不缩放的组合时才退而求其次。

    返回 (font, size, font_path, glyphs_dict, need_scale)
    """
    best_no_scale = None      # (最大墨迹, font, size, path, glyphs)
    best_scaled = None

    for path in font_paths:
        if not os.path.exists(path):
            continue
        for size in FONT_SIZE_CANDIDATES:
            font = ImageFont.truetype(path, size)
            glyphs = {}
            worst = 0
            overflow = False

            for ch in chars:
                ink, _bb, meta = render_ink(ch, font)
                if ink is None:
                    glyphs[ch] = bytes(32)
                    continue
                data, (w, h) = glyph_from_ink(ink, meta, allow_scale=False)
                worst = max(worst, w, h)
                if data is None:
                    overflow = True
                    break
                glyphs[ch] = data

            if not overflow:
                if best_no_scale is None or worst > best_no_scale[0]:
                    best_no_scale = (worst, font, size, path, glyphs)

    if best_no_scale is not None:
        worst, font, size, path, glyphs = best_no_scale
        return font, size, path, glyphs, False

    # 退路：允许缩放
    print(f"  !! 没有“不缩放就装得下”的字体/字号，改用缩放方案")
    for path in font_paths:
        if not os.path.exists(path):
            continue
        for size in FONT_SIZE_CANDIDATES:
            font = ImageFont.truetype(path, size)
            glyphs = {}
            worst = 0
            ok = True
            for ch in chars:
                ink, _bb, meta = render_ink(ch, font)
                if ink is None:
                    glyphs[ch] = bytes(32)
                    continue
                data, (w, h) = glyph_from_ink(ink, meta, allow_scale=True)
                if data is None:
                    ok = False
                    break
                worst = max(worst, w, h)
                glyphs[ch] = data
            if ok:
                if best_scaled is None or worst > best_scaled[0]:
                    best_scaled = (worst, font, size, path, glyphs)

    if best_scaled is None:
        raise RuntimeError("没有任何字体/字号能生成字模")

    worst, font, size, path, glyphs = best_scaled
    return font, size, path, glyphs, True


def build_glyph_table(font_paths, chars, verbose=True):
    """生成 {字符: 32 字节} 表，并做越界自检。"""
    font, size, path, glyphs, scaled = choose_font(font_paths, chars)

    if verbose:
        mode = "缩放" if scaled else "不缩放（笔画最锐利）"
        print(f"  采用: {os.path.basename(path)}  字号 {size}  [{mode}]")

    problems = []
    for ch in chars:
        ext = ink_extent(glyphs[ch])
        if ext is None:
            problems.append((ch, "空字模"))
            continue
        x0, x1, y0, y1 = ext
        if x1 - x0 + 1 > INK_MAX or y1 - y0 + 1 > INK_MAX:
            problems.append((ch, f"墨迹 {x1-x0+1}x{y1-y0+1} 超出 {INK_MAX}x{INK_MAX}"))
        if x0 < 0 or y0 < 0 or x1 > CELL - 1 or y1 > CELL - 1:
            problems.append((ch, f"越界 x{x0}..{x1} y{y0}..{y1}"))

    if problems:
        for ch, why in problems:
            print(f"  !! 自检失败: '{ch}' {why}")
        raise RuntimeError(f"{len(problems)} 个字符未通过自检")

    if verbose:
        print(f"  自检通过: {len(chars)} 个字符全部在 {INK_MAX}x{INK_MAX} 以内")
    return glyphs, size, path


def write_header(chars, glyphs, size, font_path, output_path):
    lines = []
    lines.append("// ============================================================")
    lines.append("//  cn_font_data.h —— 自动生成，请勿手动编辑")
    lines.append(f"//  字体: {os.path.basename(font_path)}  字号: {size}")
    lines.append(f"//  {len(chars)} 个字符，每个 16x16（墨迹限制 {INK_MAX}x{INK_MAX} 以内，保证不越界）")
    lines.append("//  位序: [0..15] 上半页(行0..7)，[16..31] 下半页(行8..15)，bit0 = 最上像素")
    lines.append("// ============================================================")
    lines.append("")
    lines.append("#ifndef __CN_FONT_DATA_H")
    lines.append("#define __CN_FONT_DATA_H")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("/** 字库字符总数 */")
    lines.append(f"#define CN_FONT_COUNT  {len(chars)}U")
    lines.append("")
    lines.append("/** 单字点阵尺寸（与 oled.c 的行列步进一致） */")
    lines.append(f"#define CN_FONT_CELL   {CELL}U")
    lines.append("")
    lines.append("/** 每个字符 32 字节 */")
    lines.append(f"static const uint8_t cn_font_glyph[{len(chars)}][32] = {{")
    for i, ch in enumerate(chars):
        hexb = ", ".join(f"0x{b:02X}" for b in glyphs[ch])
        lines.append(f"    /* {i:3d} U+{ord(ch):04X} '{ch}' */ {{{hexb}}},")
    lines.append("};")
    lines.append("")
    lines.append("/** Unicode 码点 -> 字形下标（按码点升序，供二分查找） */")
    lines.append("typedef struct {")
    lines.append("    uint16_t codepoint;")
    lines.append("    uint16_t index;")
    lines.append("} cn_font_entry_t;")
    lines.append("")
    lines.append(f"static const cn_font_entry_t cn_font_map[{len(chars)}] = {{")
    for i, ch in enumerate(chars):
        lines.append(f"    {{ 0x{ord(ch):04X}, {i} }},  /* '{ch}' */")
    lines.append("};")
    lines.append("")
    lines.append("#endif /* __CN_FONT_DATA_H */")

    with open(output_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


# ============================ main ===========================================

def main():
    check_only = "--check" in sys.argv
    root = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(root, OUTPUT_FILE)

    fonts = [FONT_PATH] if FONT_PATH else find_chinese_fonts()
    fonts = [f for f in fonts if f and os.path.exists(f)]
    if not fonts:
        print("错误: 找不到中文字体，请在 gen_font.py 顶部写死 FONT_PATH")
        sys.exit(1)
    print("候选字体: " + ", ".join(os.path.basename(f) for f in fonts))

    print("\n[1/3] 扫描源码字符串里的中文 ...")
    chars = find_chinese_chars(root, SRC_DIRS)
    if not chars:
        print("  没扫到任何中文字符，生成占位字库")
        chars = ["中"]
    print(f"  共 {len(chars)} 个唯一字符")

    print("\n[2/3] 渲染并自检 ...")
    glyphs, size, font_path = build_glyph_table(fonts, chars)

    print("\n[3/3] 写出字库 ...")
    if check_only:
        print("  --check 模式：只自检，不写文件")
    else:
        write_header(chars, glyphs, size, font_path, out)
        print(f"  已写出 {OUTPUT_FILE}（{len(chars)} 字，{len(chars) * 32} 字节）")
    print("\n完成。")


if __name__ == "__main__":
    main()