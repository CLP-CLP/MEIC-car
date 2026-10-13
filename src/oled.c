/**
 ******************************************************************************
 * @file    oled.c
 * @brief   SSD1306 128x64 I2C OLED 驱动（软件 I2C，PB6=SCL / PB7=SDA）
 *
 *  结构
 *      1. 软件 I2C 底层（起始 / 停止 / 写字节 / 读应答）
 *      2. SSD1306 命令与整屏刷新
 *      3. 显存与基本图元：点 / 线 / 矩形 / 填充矩形 / 圆 / 位图
 *      4. 文本：16x16 汉字 + 8x8 ASCII，可指定颜色（用于反白高亮）
 *
 *  为什么不用 HAL 硬件 I2C
 *      F103 的 I2C1 挂在 PCLK1 上（本工程 36 MHz）。跑 400 kHz 快速模式时
 *      SCL 高电平只有约 280 ns，而 I2C 规范要求 tHIGH >= 600 ns —— 时序不满足，
 *      传输会偶发失败丢掉一整块像素数据，屏上就会出现残留的半截笔画。
 *      软件 I2C 时序完全由代码控制，从根上避开这个问题；从机没接好也不会卡死。
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "oled.h"

#include <string.h>

/* ---- 编译前由 gen_font.py 生成的按需中文字库 ---- */
#include "cn_font_data.h"

/* ============================== 显存 ======================================= */

/** 1024 字节显存，按 [页(y/8) * 128 + x] 寻址，位序与 SSD1306 一致 */
static uint8_t OLED_显存[OLED_宽 * OLED_高 / 8U];

/* ============================== 软件 I2C =================================== */

/** 短延时：一个空循环约 3 个周期，够拉出 I2C 的建立/保持时间 */
static void i2c_延时(void)
{
    volatile uint8_t i;
    for (i = 0U; i < OLED_半周期延时; i++) {
        __NOP();
    }
}

static void i2c_SCL(uint8_t 高)
{
    HAL_GPIO_WritePin(OLED_SCL端口, OLED_SCL引脚,
                      (高 != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void i2c_SDA(uint8_t 高)
{
    HAL_GPIO_WritePin(OLED_SDA端口, OLED_SDA引脚,
                      (高 != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static uint8_t i2c_读SDA(void)
{
    return (HAL_GPIO_ReadPin(OLED_SDA端口, OLED_SDA引脚) == GPIO_PIN_SET) ? 1U : 0U;
}

/** I2C 起始条件：SCL 高时 SDA 由高变低 */
static void i2c_起始(void)
{
    i2c_SDA(1); i2c_SCL(1); i2c_延时();
    i2c_SDA(0); i2c_延时();
    i2c_SCL(0); i2c_延时();
}

/** I2C 停止条件：SCL 高时 SDA 由低变高 */
static void i2c_停止(void)
{
    i2c_SDA(0); i2c_延时();
    i2c_SCL(1); i2c_延时();
    i2c_SDA(1); i2c_延时();
}

/**
  * @brief  发一个字节，读从机应答
  * @retval 1 = 有应答（ACK），0 = 无应答（NACK）
  */
static uint8_t i2c_写字节(uint8_t 数据)
{
    uint8_t i;
    uint8_t 应答;

    for (i = 0U; i < 8U; i++)
    {
        i2c_SDA((数据 & 0x80U) ? 1U : 0U);
        数据 = (uint8_t)(数据 << 1);
        i2c_延时();
        i2c_SCL(1); i2c_延时();
        i2c_SCL(0); i2c_延时();
    }

    /* 第 9 个时钟：释放 SDA，看从机是否拉低 */
    i2c_SDA(1); i2c_延时();
    i2c_SCL(1); i2c_延时();
    应答 = (i2c_读SDA() == 0U) ? 1U : 0U;
    i2c_SCL(0); i2c_延时();

    return 应答;
}

/**
  * @brief  发一帧：从机地址 + 控制字节 + 数据
  * @param  控制 0x00 = 命令，0x40 = 显存数据
  * @retval 1 = 全部有应答，0 = 中途掉线
  */
static uint8_t i2c_发送(uint8_t 控制, const uint8_t *数据, uint16_t 长度)
{
    uint16_t i;
    uint8_t  好 = 1U;

    i2c_起始();
    if (i2c_写字节((uint8_t)OLED_I2C地址) == 0U) {
        好 = 0U;
    } else if (i2c_写字节(控制) == 0U) {
        好 = 0U;
    } else {
        for (i = 0U; i < 长度; i++)
        {
            if (i2c_写字节(数据[i]) == 0U) {
                好 = 0U;
                break;
            }
        }
    }
    i2c_停止();

    return 好;
}

/* ============================== SSD1306 命令 =============================== */

static void oled_命令(uint8_t 命令)
{
    (void)i2c_发送(0x00U, &命令, 1U);
}

static void oled_命令序列(const uint8_t *命令, uint16_t 个数)
{
    (void)i2c_发送(0x00U, 命令, 个数);
}

/* ============================== 初始化 ===================================== */

/** 配置 PB6/PB7 为 I2C 用的输出 */
static void oled_引脚初始化(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitStruct.Pin   = OLED_SCL引脚 | OLED_SDA引脚;
#if OLED_推挽输出
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
#else
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_OD;      /* 开漏 + 模块上拉 */
    GPIO_InitStruct.Pull  = GPIO_PULLUP;
#endif
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(OLED_SCL端口, &GPIO_InitStruct);

    i2c_SCL(1);
    i2c_SDA(1);
}

/** SSD1306 上电初始化序列（含软件复位） */
static void oled_发初始化序列(void)
{
    static const uint8_t 序列[] = {
        0xAE,                   /* 关显示，先别让残影露出来 */
        0x20, 0x00,             /* 水平寻址模式 */
        0xB0,                   /* 页地址 0 */
        0xC8,                   /* COM 扫描反向（上下颠倒就改 0xC0） */
        0x00,                   /* 低列地址 0 */
        0x10,                   /* 高列地址 0 */
        0x40,                   /* 起始行 0 */
        0x81, 0xCF,             /* 对比度 */
        0xA1,                   /* 段重映射（左右镜像就改 0xA0） */
        0xA6,                   /* 正常显示，不反色 */
        0xA8, 0x3F,             /* 多路复用 1/64 */
        0xA4,                   /* 显示跟随显存 */
        0xD3, 0x00,             /* 显示偏移 0 */
        0xD5, 0x80,             /* 时钟分频 */
        0xD9, 0xF1,             /* 预充电周期 */
        0xDA, 0x12,             /* COM 引脚配置 */
        0xDB, 0x40,             /* VCOMH */
        0x8D, 0x14,             /* 使能电荷泵 */
        0xAF                    /* 开显示 */
    };
    static const uint8_t 软件复位[] = { 0xE2 };

    oled_命令序列(软件复位, 1U);
    HAL_Delay(10U);
    oled_命令序列(序列, (uint16_t)sizeof(序列));
}

void OLED_初始化(void)
{
    oled_引脚初始化();
    oled_发初始化序列();
    memset(OLED_显存, 0x00, sizeof(OLED_显存));
    OLED_刷新();
}

uint8_t OLED_是否在线(void)
{
    uint8_t 应答;

    i2c_起始();
    应答 = i2c_写字节((uint8_t)OLED_I2C地址);
    i2c_停止();

    return 应答;
}

/* ============================== 显示控制 =================================== */

void OLED_清屏(void)
{
    memset(OLED_显存, 0x00, sizeof(OLED_显存));
}

/* ============================== 分片刷新状态 =============================== */

/** 分片刷新：已经送出去的字节数（送完清 0） */
static uint16_t OLED_刷新偏移 = 0U;

/** 1 = 正在分片送一帧，0 = 没在送 */
static uint8_t  OLED_分片中   = 0U;

void OLED_刷新(void)
{
    static const uint8_t 列地址[] = { 0x21, 0x00, 0x7F };
    static const uint8_t 页地址[] = { 0x22, 0x00, 0x07 };
    uint16_t 已发 = 0U;

    /* 设定窗口：列 0..127，页 0..7，然后一次把 1024 字节显存送过去 */
    oled_命令序列(列地址, (uint16_t)sizeof(列地址));
    oled_命令序列(页地址, (uint16_t)sizeof(页地址));

    /* 分块发送，单次传输不要太长；某块失败就重发一次 */
    while (已发 < (uint16_t)sizeof(OLED_显存))
    {
        uint16_t 本块 = (uint16_t)(sizeof(OLED_显存) - 已发);
        if (本块 > 128U) {
            本块 = 128U;
        }

        if (i2c_发送(0x40U, &OLED_显存[已发], 本块) == 0U) {
            (void)i2c_发送(0x40U, &OLED_显存[已发], 本块);
        }
        已发 = (uint16_t)(已发 + 本块);
    }

    /* 整帧送完：分片状态一并复位，免得半截状态和这里打架 */
    OLED_刷新偏移 = 0U;
    OLED_分片中   = 0U;
}

void OLED_开始分片刷新(void)
{
    static const uint8_t 列地址[] = { 0x21, 0x00, 0x7F };
    static const uint8_t 页地址[] = { 0x22, 0x00, 0x07 };

    /* 窗口设成整屏。之后 SSD1306 的地址指针会自己往下走，
       所以后面每次续传都不用再发地址，接着发数据即可。 */
    oled_命令序列(列地址, (uint16_t)sizeof(列地址));
    oled_命令序列(页地址, (uint16_t)sizeof(页地址));

    OLED_刷新偏移 = 0U;
    OLED_分片中   = 1U;
}

uint8_t OLED_刷新分片(uint16_t 最多字节)
{
    uint16_t 剩余;

    if (OLED_分片中 == 0U) {
        return 1U;                  /* 没在送帧，当作"已经送完" */
    }

    剩余 = (uint16_t)(sizeof(OLED_显存) - (uint32_t)OLED_刷新偏移);
    if (剩余 == 0U) {
        OLED_分片中 = 0U;
        return 1U;
    }

    if (最多字节 == 0U) {
        最多字节 = 1U;
    }
    if (最多字节 > 剩余) {
        最多字节 = 剩余;
    }

    /* 某一片发失败就重发一次（和整帧刷新一样的容错） */
    if (i2c_发送(0x40U, &OLED_显存[OLED_刷新偏移], 最多字节) == 0U) {
        (void)i2c_发送(0x40U, &OLED_显存[OLED_刷新偏移], 最多字节);
    }

    OLED_刷新偏移 = (uint16_t)(OLED_刷新偏移 + 最多字节);

    if (OLED_刷新偏移 >= (uint16_t)sizeof(OLED_显存)) {
        OLED_刷新偏移 = 0U;
        OLED_分片中   = 0U;
        return 1U;
    }

    return 0U;
}

void OLED_清屏并刷新(void)
{
    OLED_清屏();
    OLED_刷新();
}

void OLED_开显示(uint8_t 开)
{
    oled_命令((开 != 0U) ? 0xAFU : 0xAEU);
}

void OLED_反色(uint8_t 反)
{
    oled_命令((反 != 0U) ? 0xA7U : 0xA6U);
}

void OLED_设置亮度(uint8_t 亮度)
{
    oled_命令(0x81U);
    oled_命令(亮度);
}

/* ============================== 基本图元 =================================== */

void OLED_画点(int16_t x, int16_t y, uint8_t 颜色)
{
    uint8_t 掩码;

    if (x < 0 || y < 0 || x >= OLED_宽 || y >= OLED_高) {
        return;
    }

    掩码 = (uint8_t)(1U << (y & 7));
    if (颜色 != 0U) {
        OLED_显存[((uint16_t)(y >> 3) * OLED_宽) + (uint16_t)x] |= 掩码;
    } else {
        OLED_显存[((uint16_t)(y >> 3) * OLED_宽) + (uint16_t)x] &= (uint8_t)(~掩码);
    }
}

void OLED_画线(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t 颜色)
{
    int16_t dx = (int16_t)((x1 > x0) ? (x1 - x0) : (x0 - x1));
    int16_t dy = (int16_t)((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int16_t sx = (x0 < x1) ? 1 : -1;
    int16_t sy = (y0 < y1) ? 1 : -1;
    int16_t 误差 = (int16_t)(dx - dy);

    for (;;)
    {
        int16_t e2;

        OLED_画点(x0, y0, 颜色);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        e2 = (int16_t)(误差 << 1);
        if (e2 > -dy) {
            误差 = (int16_t)(误差 - dy);
            x0 = (int16_t)(x0 + sx);
        }
        if (e2 < dx) {
            误差 = (int16_t)(误差 + dx);
            y0 = (int16_t)(y0 + sy);
        }
    }
}

void OLED_填充矩形(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t 颜色)
{
    int16_t x, y;

    if (x0 > x1) { int16_t t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int16_t t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > (OLED_宽 - 1)) { x1 = OLED_宽 - 1; }
    if (y1 > (OLED_高 - 1)) { y1 = OLED_高 - 1; }

    for (y = y0; y <= y1; y++)
    {
        for (x = x0; x <= x1; x++) {
            OLED_画点(x, y, 颜色);
        }
    }
}

void OLED_填充灰阶矩形(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t 灰度)
{
    /*
     *  预先把 8x8 有序抖动矩阵（Bayer）换算成"点亮的门槛值"（0~1000）。
     *
     *  原矩阵里 0~63 每种数字各出现一次，门槛 = (值 × 2 + 1) × 1000 / 128，
     *  于是"灰度 ≥ 门槛就点亮"这个判断，直接就是"64 个位置里点亮若干个"，
     *  而且点亮的位置均匀撒开：
     *
     *      灰度   0~   7  -> 一个都不亮（全灭 = 纯白底）
     *      灰度 500       -> 正好点亮 32 个（中灰 = 判黑阈值）
     *      灰度 992~1000  -> 64 个全亮（纯黑线）
     *
     *  ×2+1 是取每段的**中点**，保证灰度 0 时一个不亮、1000 时全亮，
     *  两端不会留下"半亮"的毛边。
     *
     *  为什么不直接把 Bayer 原值存进来、运行时再算：一块灰阶框有几百个点，
     *  而 Cortex-M3 没有硬件除法，每次 `×1000÷128` 要几十个周期 —— 查这张
     *  现成的表就只剩一次比较。主屏幕每 100 ms 重画一帧，这笔开销不能白花。
     *
     *  为什么用 8x8 而不是 4x4：4x4 只有 17 级，灰度 50% 时是规规矩矩的
     *  棋盘格，一眼就看出是"点阵"而不是"灰"；8x8 有 65 级，图案细得多，
     *  在小框里看起来就是连续的一块灰。
     */
    static const uint16_t 抖动门槛[8][8] = {
        {   8, 508, 133, 633,  39, 539, 164, 664 },
        { 758, 258, 883, 383, 789, 289, 914, 414 },
        { 195, 695,  70, 570, 227, 727, 102, 602 },
        { 945, 445, 820, 320, 977, 477, 852, 352 },
        {  55, 555, 180, 680,  23, 523, 148, 648 },
        { 805, 305, 930, 430, 773, 273, 898, 398 },
        { 242, 742, 117, 617, 211, 711,  86, 586 },
        { 992, 492, 867, 367, 961, 461, 836, 336 },
    };
    int16_t x, y;

    if (x0 > x1) { int16_t t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int16_t t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > (OLED_宽 - 1)) { x1 = OLED_宽 - 1; }
    if (y1 > (OLED_高 - 1)) { y1 = OLED_高 - 1; }
    if (灰度 > 1000U) { 灰度 = 1000U; }

    for (y = y0; y <= y1; y++)
    {
        /* 直接写显存（不走 OLED_画点），一块灰阶框几百个点，省下函数调用开销 */
        uint16_t 行基 = (uint16_t)((y >> 3) * OLED_宽);
        uint8_t  位   = (uint8_t)(1U << (y & 7));

        for (x = x0; x <= x1; x++)
        {
            if (灰度 >= 抖动门槛[y & 7][x & 7]) {
                OLED_显存[行基 + x] |= 位;
            }
        }
    }
}

void OLED_画矩形(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t 颜色)
{
    OLED_画线(x0, y0, x1, y0, 颜色);
    OLED_画线(x0, y1, x1, y1, 颜色);
    OLED_画线(x0, y0, x0, y1, 颜色);
    OLED_画线(x1, y0, x1, y1, 颜色);
}

void OLED_画圆(int16_t 圆心x, int16_t 圆心y, int16_t 半径, uint8_t 颜色)
{
    int16_t x = 0;
    int16_t y = 半径;
    int16_t d = (int16_t)(3 - (半径 << 1));

    while (x <= y)
    {
        OLED_画点((int16_t)(圆心x + x), (int16_t)(圆心y + y), 颜色);
        OLED_画点((int16_t)(圆心x - x), (int16_t)(圆心y + y), 颜色);
        OLED_画点((int16_t)(圆心x + x), (int16_t)(圆心y - y), 颜色);
        OLED_画点((int16_t)(圆心x - x), (int16_t)(圆心y - y), 颜色);
        OLED_画点((int16_t)(圆心x + y), (int16_t)(圆心y + x), 颜色);
        OLED_画点((int16_t)(圆心x - y), (int16_t)(圆心y + x), 颜色);
        OLED_画点((int16_t)(圆心x + y), (int16_t)(圆心y - x), 颜色);
        OLED_画点((int16_t)(圆心x - y), (int16_t)(圆心y - x), 颜色);

        if (d < 0) {
            d = (int16_t)(d + (x << 2) + 6);
        } else {
            d = (int16_t)(d + ((x - y) << 2) + 10);
            y--;
        }
        x++;
    }
}

void OLED_画位图(int16_t x, int16_t y, uint8_t 宽, uint8_t 高, const uint8_t *位图)
{
    uint8_t 列, 行;

    for (列 = 0U; 列 < 宽; 列++)
    {
        for (行 = 0U; 行 < 高; 行++)
        {
            uint8_t 页 = (uint8_t)(行 >> 3);
            uint8_t 位 = (uint8_t)(行 & 7U);
            uint8_t 字节 = 位图[(uint16_t)页 * 宽 + 列];

            if ((字节 & (uint8_t)(1U << 位)) != 0U) {
                OLED_画点((int16_t)(x + 列), (int16_t)(y + 行), OLED_亮);
            }
        }
    }
}

/* ============================== 8x8 ASCII 字库 ============================= */
/* Public Domain font8x8_basic，U+0000~U+007F。每字符 8 字节 = 8 行，
   每字节一行 8 像素、bit0 在最左。绘制时转置成 SSD1306 的列序。 */

static const uint8_t font8x8[128][8] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x00 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x01 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x02 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x03 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x04 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x05 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x06 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x07 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x08 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x09 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x0A */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x0B */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x0C */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x0D */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x0E */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x0F */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x10 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x11 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x12 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x13 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x14 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x15 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x16 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x17 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x18 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x19 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x1A */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x1B */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x1C */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x1D */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x1E */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x1F */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x20 空格 */
    { 0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},   /* 0x21 ! */
    { 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x22 " */
    { 0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},   /* 0x23 # */
    { 0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00},   /* 0x24 $ */
    { 0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00},   /* 0x25 % */
    { 0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00},   /* 0x26 & */
    { 0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x27 ' */
    { 0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00},   /* 0x28 ( */
    { 0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00},   /* 0x29 ) */
    { 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00},   /* 0x2A * */
    { 0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00},   /* 0x2B + */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06},   /* 0x2C , */
    { 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00},   /* 0x2D - */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00},   /* 0x2E . */
    { 0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00},   /* 0x2F / */
    { 0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00},   /* 0x30 0 */
    { 0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},   /* 0x31 1 */
    { 0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00},   /* 0x32 2 */
    { 0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},   /* 0x33 3 */
    { 0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00},   /* 0x34 4 */
    { 0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},   /* 0x35 5 */
    { 0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00},   /* 0x36 6 */
    { 0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},   /* 0x37 7 */
    { 0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00},   /* 0x38 8 */
    { 0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},   /* 0x39 9 */
    { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00},   /* 0x3A : */
    { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06},   /* 0x3B ; */
    { 0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00},   /* 0x3C < */
    { 0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00},   /* 0x3D = */
    { 0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00},   /* 0x3E > */
    { 0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00},   /* 0x3F ? */
    { 0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00},   /* 0x40 @ */
    { 0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},   /* 0x41 A */
    { 0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00},   /* 0x42 B */
    { 0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},   /* 0x43 C */
    { 0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00},   /* 0x44 D */
    { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},   /* 0x45 E */
    { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00},   /* 0x46 F */
    { 0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},   /* 0x47 G */
    { 0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00},   /* 0x48 H */
    { 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   /* 0x49 I */
    { 0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00},   /* 0x4A J */
    { 0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},   /* 0x4B K */
    { 0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00},   /* 0x4C L */
    { 0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},   /* 0x4D M */
    { 0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00},   /* 0x4E N */
    { 0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},   /* 0x4F O */
    { 0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00},   /* 0x50 P */
    { 0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},   /* 0x51 Q */
    { 0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00},   /* 0x52 R */
    { 0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},   /* 0x53 S */
    { 0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   /* 0x54 T */
    { 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},   /* 0x55 U */
    { 0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},   /* 0x56 V */
    { 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},   /* 0x57 W */
    { 0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00},   /* 0x58 X */
    { 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},   /* 0x59 Y */
    { 0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00},   /* 0x5A Z */
    { 0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00},   /* 0x5B [ */
    { 0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00},   /* 0x5C \ */
    { 0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00},   /* 0x5D ] */
    { 0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00},   /* 0x5E ^ */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},   /* 0x5F _ */
    { 0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x60 ` */
    { 0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00},   /* 0x61 a */
    { 0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00},   /* 0x62 b */
    { 0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00},   /* 0x63 c */
    { 0x38, 0x30, 0x30, 0x3E, 0x33, 0x33, 0x6E, 0x00},   /* 0x64 d */
    { 0x00, 0x00, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0x00},   /* 0x65 e */
    { 0x1C, 0x36, 0x06, 0x0F, 0x06, 0x06, 0x0F, 0x00},   /* 0x66 f */
    { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F},   /* 0x67 g */
    { 0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00},   /* 0x68 h */
    { 0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   /* 0x69 i */
    { 0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E},   /* 0x6A j */
    { 0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00},   /* 0x6B k */
    { 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   /* 0x6C l */
    { 0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00},   /* 0x6D m */
    { 0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00},   /* 0x6E n */
    { 0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00},   /* 0x6F o */
    { 0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F},   /* 0x70 p */
    { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78},   /* 0x71 q */
    { 0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00},   /* 0x72 r */
    { 0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00},   /* 0x73 s */
    { 0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00},   /* 0x74 t */
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00},   /* 0x75 u */
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},   /* 0x76 v */
    { 0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00},   /* 0x77 w */
    { 0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00},   /* 0x78 x */
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F},   /* 0x79 y */
    { 0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00},   /* 0x7A z */
    { 0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00},   /* 0x7B { */
    { 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00},   /* 0x7C | */
    { 0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00},   /* 0x7D } */
    { 0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x7E ~ */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* 0x7F */
};

/* ============================== 字库查找 =================================== */

/** 二分查找码点对应的字形下标，找不到返回 -1 */
static int16_t 字库_查字形(uint16_t 码点)
{
    int16_t lo = 0;
    int16_t hi = (int16_t)(CN_FONT_COUNT - 1U);

    while (lo <= hi)
    {
        int16_t mid = (int16_t)(((uint16_t)lo + (uint16_t)hi) >> 1);
        uint16_t val = cn_font_map[mid].codepoint;

        if (码点 < val) {
            hi = (int16_t)(mid - 1);
        } else if (码点 > val) {
            lo = (int16_t)(mid + 1);
        } else {
            return (int16_t)cn_font_map[mid].index;
        }
    }
    return -1;
}

/** 从 UTF-8 串取一个码点，返回消耗的字节数（1~3） */
static uint8_t 字库_取码点(const char *s, uint16_t *码点)
{
    uint8_t c = (uint8_t)s[0];

    if (c < 0x80U) {
        *码点 = (uint16_t)c;
        return 1U;
    }
    if ((c & 0xE0U) == 0xC0U) {
        *码点 = (uint16_t)(((uint16_t)(c & 0x1FU) << 6)
                         |  (uint16_t)((uint8_t)s[1] & 0x3FU));
        return 2U;
    }
    if ((c & 0xF0U) == 0xE0U) {
        *码点 = (uint16_t)(((uint16_t)(c & 0x0FU) << 12)
                         | ((uint16_t)((uint8_t)s[1] & 0x3FU) << 6)
                         |  (uint16_t)((uint8_t)s[2] & 0x3FU));
        return 3U;
    }
    *码点 = 0U;
    return 1U;
}

/* ============================== 文本绘制 =================================== */

/**
  * @brief  画一个 8x8 ASCII 字符
  * @note   只写自己那 8x8 区域，不碰相邻字符
  */
static void 文本_画ASCII(int16_t x, int16_t y, char 字符, uint8_t 颜色)
{
    const uint8_t *字形 = font8x8[(uint8_t)字符 & 0x7FU];
    uint8_t 列, 行;

    for (列 = 0U; 列 < 8U; 列++)
    {
        for (行 = 0U; 行 < 8U; 行++)
        {
            if ((字形[行] & (uint8_t)(1U << 列)) != 0U) {
                OLED_画点((int16_t)(x + 列), (int16_t)(y + 行), 颜色);
            }
        }
    }
}

/**
  * @brief  画一个 16x16 汉字
  * @note   字模正好 16x16，越界部分由 OLED_画点() 裁掉
  */
static void 文本_画汉字(int16_t x, int16_t y, uint16_t 字形下标, uint8_t 颜色)
{
    const uint8_t *字形;
    uint8_t 列, 行;

    if (字形下标 >= CN_FONT_COUNT) {
        return;
    }
    字形 = cn_font_glyph[字形下标];

    for (列 = 0U; 列 < 16U; 列++)
    {
        uint8_t 上半 = 字形[列];
        uint8_t 下半 = 字形[16U + 列];

        for (行 = 0U; 行 < 8U; 行++)
        {
            if ((上半 & (uint8_t)(1U << 行)) != 0U) {
                OLED_画点((int16_t)(x + 列), (int16_t)(y + 行), 颜色);
            }
            if ((下半 & (uint8_t)(1U << 行)) != 0U) {
                OLED_画点((int16_t)(x + 列), (int16_t)(y + 8 + 行), 颜色);
            }
        }
    }
}

void OLED_显示中文串色(int16_t x, int16_t y, const char *utf8, uint8_t 颜色)
{
    int16_t 当前位置 = x;

    while (*utf8 != '\0')
    {
        uint16_t 码点;
        uint8_t  步进 = 字库_取码点(utf8, &码点);

        if (码点 < 0x80U)
        {
            /* ASCII：8x8，在 16 像素行内垂直居中（下移 4 像素） */
            if ((当前位置 + 8) > OLED_宽) {
                break;
            }
            文本_画ASCII(当前位置, (int16_t)(y + 4), (char)码点, 颜色);
            当前位置 = (int16_t)(当前位置 + 8);
        }
        else
        {
            /* 汉字：16x16，占满整行 */
            if ((当前位置 + 16) > OLED_宽) {
                break;
            }
            {
                int16_t 下标 = 字库_查字形(码点);
                if (下标 >= 0) {
                    文本_画汉字(当前位置, y, (uint16_t)下标, 颜色);
                }
            }
            当前位置 = (int16_t)(当前位置 + 16);
        }

        utf8 += 步进;
    }
}

void OLED_显示中文串(int16_t x, int16_t y, const char *utf8)
{
    OLED_显示中文串色(x, y, utf8, OLED_亮);
}

void OLED_显示ASCII串色(int16_t x, int16_t y, const char *s, uint8_t 颜色)
{
    int16_t 当前位置 = x;

    while (*s != '\0')
    {
        if ((当前位置 + 8) > OLED_宽) {
            break;
        }
        文本_画ASCII(当前位置, y, *s, 颜色);
        当前位置 = (int16_t)(当前位置 + 8);
        s++;
    }
}

void OLED_显示ASCII串(int16_t x, int16_t y, const char *s)
{
    OLED_显示ASCII串色(x, y, s, OLED_亮);
}

uint8_t OLED_文本宽度(const char *utf8)
{
    uint16_t 宽 = 0U;

    while (*utf8 != '\0')
    {
        uint16_t 码点;
        uint8_t  步进 = 字库_取码点(utf8, &码点);

        宽 += (码点 < 0x80U) ? 8U : 16U;
        if (宽 >= OLED_宽) {
            return OLED_宽;
        }
        utf8 += 步进;
    }
    return (uint8_t)宽;
}

void OLED_显示数字(int16_t x, int16_t y, int32_t 值)
{
    char     缓冲[12];
    uint8_t  i = 0U;
    uint8_t  j;
    uint32_t 绝对值;
    uint8_t  负数 = 0U;

    if (值 < 0) {
        负数 = 1U;
        绝对值 = (uint32_t)(-(值));
    } else {
        绝对值 = (uint32_t)值;
    }

    do {
        缓冲[i++] = (char)('0' + (绝对值 % 10U));
        绝对值 /= 10U;
    } while ((绝对值 != 0U) && (i < 10U));

    if (负数 != 0U) {
        缓冲[i++] = '-';
    }

    /* 反转成正常顺序 */
    for (j = 0U; j < (uint8_t)(i / 2U); j++)
    {
        char t = 缓冲[j];
        缓冲[j] = 缓冲[i - 1U - j];
        缓冲[i - 1U - j] = t;
    }
    缓冲[i] = '\0';

    OLED_显示ASCII串(x, (int16_t)(y + 4), 缓冲);
}

void OLED_显示十六进制(int16_t x, int16_t y, uint32_t 值, uint8_t 位数)
{
    static const char 十六进制字符[] = "0123456789ABCDEF";
    char 缓冲[9];
    uint8_t i;

    if (位数 == 0U) { 位数 = 1U; }
    if (位数 > 8U)  { 位数 = 8U; }

    for (i = 0U; i < 位数; i++)
    {
        缓冲[位数 - 1U - i] = 十六进制字符[值 & 0x0FU];
        值 >>= 4;
    }
    缓冲[位数] = '\0';

    OLED_显示ASCII串(x, (int16_t)(y + 4), 缓冲);
}

/* ============================== 兼容旧接口 ================================= */

void 显示_初始化(void)
{
    OLED_初始化();
}

void 显示_清屏(void)
{
    OLED_清屏();
}

void 显示_刷新(void)
{
    OLED_刷新();
}

void 显示_开始刷新(void)
{
    OLED_开始分片刷新();
}

uint8_t 显示_刷新分片(uint16_t 最多字节)
{
    return OLED_刷新分片(最多字节);
}

void 显示_画字符(uint8_t 列, uint8_t 行, char 字符)
{
    文本_画ASCII((int16_t)列 * 8, (int16_t)行 * 8, 字符, OLED_亮);
}

void 显示_画字符串(uint8_t 列, uint8_t 行, const char *串)
{
    OLED_显示ASCII串((int16_t)列 * 8, (int16_t)行 * 8, 串);
}

void 显示_画中文(uint8_t 列, uint8_t 行, const char *中英文字符串)
{
    OLED_显示中文串((int16_t)列 * 8, (int16_t)行 * 8, 中英文字符串);
}

void 显示_画空心框(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
    OLED_画矩形(x0, y0, x1, y1, OLED_亮);
}

/* ============================== 自检画面 =================================== */

void OLED_显示自检画面(void)
{
    uint8_t 行;

    OLED_清屏();

    /* 标题反白条 */
    OLED_填充矩形(0, 0, OLED_宽 - 1, 15, OLED_亮);
    OLED_显示中文串色(2, 0, "屏幕自检", OLED_灭);

    /* 中英混排 + 数字 */
    OLED_显示中文串(0, 16, "汉字");
    OLED_显示ASCII串(40, 20, "ABC abc 0123");
    OLED_显示数字(0, 32, -1234);

    /* 边框 + 圆 */
    OLED_画矩形(0, 48, 63, 63, OLED_亮);
    OLED_画圆(96, 56, 7, OLED_亮);

    /* 右边逐行小字 */
    for (行 = 0U; 行 < 3U; 行++)
    {
        char 行文字[6];
        行文字[0] = 'L';
        行文字[1] = (char)('0' + 行);
        行文字[2] = ':';
        行文字[3] = 'o';
        行文字[4] = 'k';
        行文字[5] = '\0';
        OLED_显示ASCII串(72, (int16_t)(16 + 行 * 8), 行文字);
    }

    OLED_刷新();
}
