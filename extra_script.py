# extra_script.py —— PlatformIO 构建脚本
#
# 1) 编译前自动运行 gen_font.py 扫描源码中的中文字符并生成 16×16 字库
# 2) 编译后额外生成 firmware.hex（方便 ST-Link Utility 等工具烧录）

Import("env")

# =========================== 预构建：生成中文字库 ============================

def _run_gen_font(source, target, env):
    """编译前执行 gen_font.py"""
    import subprocess
    import os

    script = os.path.join(env.subst("$PROJECT_DIR"), "gen_font.py")
    print("\n==============================================")
    print("  gen_font.py: 扫描中文 → 生成 16x16 字库 ...")
    print("==============================================")
    result = subprocess.run(
        [r"C:\Users\CLP\AppData\Local\Programs\Python\Python314\python.exe", script],
        cwd=env.subst("$PROJECT_DIR"),
        capture_output=False,
    )
    if result.returncode != 0:
        print("!! gen_font.py 执行失败，继续编译（可能缺中文字体）")
    print()

env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", _run_gen_font)

# =========================== 后构建：生成 .hex ===============================

env.AddPostAction(
    "$BUILD_DIR/${PROGNAME}.elf",
    env.VerboseAction(
        "$OBJCOPY -O ihex "
        "$BUILD_DIR/${PROGNAME}.elf "
        "$BUILD_DIR/${PROGNAME}.hex",
        "Building $BUILD_DIR/${PROGNAME}.hex",
    ),
)