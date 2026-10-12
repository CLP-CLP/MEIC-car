# extra_script.py —— PlatformIO 构建后额外生成 Intel HEX 文件
#
# 说明：PlatformIO 默认只产出 firmware.elf / firmware.bin，
#       而 ST-Link Utility、FlyMcu(mcuisp)、STM32CubeProgrammer 等工具
#       在“串口 ISP / 手动下载”场景下更常用 .hex。
#
# 产出位置: .pio/build/<env>/firmware.hex

Import("env")

env.AddPostAction(
    "$BUILD_DIR/${PROGNAME}.elf",
    env.VerboseAction(
        "$OBJCOPY -O ihex "
        "$BUILD_DIR/${PROGNAME}.elf "
        "$BUILD_DIR/${PROGNAME}.hex",
        "Building $BUILD_DIR/${PROGNAME}.hex",
    ),
)
