#!/usr/bin/env python3
"""
===============================================================================
 gen_font.py —— 扫描源码中的中文字符，自动生成 16x16 点阵字库头文件

 用法：
     python gen_font.py

 在 PlatformIO 编译前自动运行（通过 extra_script.py 的预构建钩子）。

 原理：
     1. 扫描 src/ 和 include/ 下所有 .c / .h 文件，提取唯一中文字符
     2. 用系统自带的中文字体（宋体/微软雅黑等）渲染为 16x16 单色位图
     3. 转为 SSD1306 列序格式（每字节 = 一列 8 像素）写入 cn_font_data.h
     4. 同时生成 Unicode 码点 -> 索引的映射表，C 代码用二分查找

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
    print("╔══════════════════════════════════════════════════════════════╗")
    print("║  缺少 Pillow 库，请先安装：                                  ║")
    print("║      pip install pillow                                     ║")
    print("╚══════════════════════════════════════════════════════════════╝")
    sys.exit(1)

# ============================ 配置 ===========================================

SRC_DIRS = ["src", "include"]       # 扫描哪些目录
OUTPUT_FILE = "src/cn_font_data.h"  # 输出头文件
FONT_SIZE = 12                      # 字号（点数，12pt 更紧凑，Marlin 风格）
FONT_PATH = None                    # None = 自动查找系统字体

# 备选字体列表（Windows 按优先级排列）
_WIN_FONTS = [
    "C:/Windows/Fonts/simsun.ttc",       # 宋体（最标准的点阵风格）
    "C:/Windows/Fonts/simhei.ttf",       # 黑体
    "C:/Windows/Fonts/msyh.ttc",         # 微软雅黑
    "C:/Windows/Fonts/Deng.ttf",         # 等线
]

_LINUX_FONTS = [
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
]

# ============================ 字体查找 ========================================

def find_chinese_font():
    """自动查找系统可用的中文字体"""
    import platform

    candidates = _WIN_FONTS if platform.system() == "Windows" else _LINUX_FONTS

    for f in candidates:
        if os.path.exists(f):
            return f

    # 最后尝试用 PIL 的默认字体（通常不支持中文，但试试无妨）
    print("警告: 未找到系统中文字体，尝试使用 PIL 默认字体（可能无法渲染中文）")
    return None

# ============================ 字符扫描 ========================================

def find_chinese_chars(project_root, dirs):
    """
    扫描指定目录下所有 .c / .h 文件中的字符串字面量，
    提取其中出现的唯一中文字符（仅限 OLED 显示用的，不是注释里的）。

    返回排序后的字符列表。
    """
    chars = set()
    # 匹配 CJK 统一汉字 + 中文标点
    cn_pattern = re.compile(r'[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef]')
    # 匹配 C 字符串字面量："..."  ，处理转义引号
    str_pattern = re.compile(r'"((?:[^"\\]|\\.)*)"')

    for d in dirs:
        full_dir = os.path.join(project_root, d)
        if not os.path.isdir(full_dir):
            continue
        for root, _, files in os.walk(full_dir):
            for f in files:
                if not f.endswith(('.c', '.h')):
                    continue
                path = os.path.join(root, f)
                try:
                    with open(path, "r", encoding="utf-8") as fp:
                        content = fp.read()
                except Exception as e:
                    print(f"  警告: 读取 {path} 失败: {e}")
                    continue

                # 只提取字符串字面量里的中文（忽略注释和代码中的中文标识符）
                file_chars = set()
                for m in str_pattern.finditer(content):
                    s = m.group(1)
                    file_chars.update(cn_pattern.findall(s))

                if file_chars:
                    rel = os.path.relpath(path, project_root)
                    print(f"  {rel}: {len(file_chars)} 个显示用汉字")
                chars.update(file_chars)

    return sorted(chars)

# ============================ 位图渲染 ========================================

def render_char(char, font, size=16):
    """
    渲染单个汉字为 16x16 单色位图，返回 32 字节的 SSD1306 列序数据。

    SSD1306 列序格式（对应 SSD1306 水平寻址模式）：
        前 16 字节 = 列 0..15 的 上半页（行 0..7），每字节 bit0=最上像素
        后 16 字节 = 列 0..15 的 下半页（行 8..15），每字节 bit0=最上像素

    绘制时直接 memcpy 到帧缓冲对应位置即可，无需转置。
    """
    img = Image.new("L", (size, size), 0)  # 黑底
    draw = ImageDraw.Draw(img)

    # 渲染字符，尝试让字形居中
    draw.text((0, 0), char, font=font, fill=255)

    data = bytearray(32)
    for col in range(16):
        byte_top = 0  # 行 0..7
        byte_bot = 0  # 行 8..15
        for row in range(8):
            if img.getpixel((col, row)) > 128:
                byte_top |= 1 << row
            if img.getpixel((col, row + 8)) > 128:
                byte_bot |= 1 << row
        data[col] = byte_top
        data[col + 16] = byte_bot

    return bytes(data)

# ============================ 头文件生成 ======================================

def generate_header(chars, font_path, output_path):
    """生成 cn_font_data.h"""
    font = ImageFont.truetype(font_path, FONT_SIZE)

    lines = []
    lines.append("// ============================================================")
    lines.append("//  cn_font_data.h —— 自动生成，请勿手动编辑")
    lines.append(f"//  扫描源码得到 {len(chars)} 个中文字符，16x16 列序点阵")
    lines.append(f"//  生成时间: {__import__('datetime').datetime.now()}")
    lines.append("// ============================================================")
    lines.append("")
    lines.append("#ifndef __CN_FONT_DATA_H")
    lines.append("#define __CN_FONT_DATA_H")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append(f"/** 字库字符总数 */")
    lines.append(f"#define CN_FONT_COUNT  {len(chars)}U")
    lines.append("")
    lines.append("/** 每个字符 32 字节：前 16 字节 = 上半页，后 16 字节 = 下半页 */")
    lines.append(f"static const uint8_t cn_font_glyph[{len(chars)}][32] = {{")

    for i, ch in enumerate(chars):
        bitmap = render_char(ch, font)
        hex_bytes = ", ".join(f"0x{b:02X}" for b in bitmap)
        lines.append(f"    /* {i:3d} U+{ord(ch):04X} '{ch}' */ {{{hex_bytes}}},")

    lines.append("};")
    lines.append("")
    lines.append("/** Unicode 码点 -> 字形索引（按码点升序排列，供二分查找） */")
    lines.append("typedef struct {")
    lines.append("    uint16_t codepoint;  /**< Unicode 码点 */")
    lines.append("    uint8_t  index;      /**< cn_font_glyph 中的索引 */")
    lines.append("} cn_font_entry_t;")
    lines.append("")
    lines.append(f"static const cn_font_entry_t cn_font_map[{len(chars)}] = {{")

    for i, ch in enumerate(chars):
        lines.append(f"    {{ 0x{ord(ch):04X}, {i} }},  /* '{ch}' */")

    lines.append("};")
    lines.append("")
    lines.append("#endif /* __CN_FONT_DATA_H */")

    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    print(f"\n  已生成 {output_path}（{len(chars)} 个汉字，{len(chars) * 32} 字节）")

# ============================ main ============================================

def main():
    project_root = os.path.dirname(os.path.abspath(__file__))
    output_path = os.path.join(project_root, OUTPUT_FILE)

    # 1. 找字体
    font_path = FONT_PATH or find_chinese_font()
    if not font_path:
        print("错误: 找不到任何中文字体，无法生成字库")
        print("请手动设置 gen_font.py 顶部的 FONT_PATH 变量指向一个 .ttf/.ttc 文件")
        sys.exit(1)
    print(f"字体: {font_path}")

    # 2. 扫描中文字符
    print("\n扫描源码中的中文字符...")
    chars = find_chinese_chars(project_root, SRC_DIRS)

    if not chars:
        print("未找到任何中文字符，生成空字库占位文件")
        # 生成一个空壳头文件，让编译不报错
        dummy = [
            "#ifndef __CN_FONT_DATA_H",
            "#define __CN_FONT_DATA_H",
            "#include <stdint.h>",
            "#define CN_FONT_COUNT  0U",
            "static const uint8_t cn_font_glyph[1][32] = {{0}};",
            "typedef struct { uint16_t codepoint; uint8_t index; } cn_font_entry_t;",
            "static const cn_font_entry_t cn_font_map[1] = {{0, 0}};",
            "#endif",
        ]
        os.makedirs(os.path.dirname(output_path), exist_ok=True)
        with open(output_path, "w", encoding="utf-8") as f:
            f.write("\n".join(dummy) + "\n")
        print(f"  已生成空字库 {output_path}")
        return

    print(f"\n共找到 {len(chars)} 个唯一字符: {''.join(chars)}")

    # 3. 渲染并生成头文件
    print("\n渲染字模...")
    generate_header(chars, font_path, output_path)

    print("\n完成！")

if __name__ == "__main__":
    main()