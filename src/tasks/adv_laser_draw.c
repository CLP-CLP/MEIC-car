/**
 ******************************************************************************
 * @file    adv_laser_draw.c
 * @brief   发挥任务三：二维云台激光绘图（纯整数+查表，零浮点）
 *
 *  硬件：
 *    云台舵机 PB8 (pan 水平) / PB9 (tilt 垂直)
 *    激光笔   PB5 (推挽输出，高电平点亮)
 *
 *  坐标映射（归一化 → 舵机角度）：
 *    pan  中心 85°，跨度 ±30°
 *    tilt 中心 40°，跨度 ±20°
 *
 *  使用 256 点正弦查找表避免 math.h，Flash 节省约 8 KB。
 ******************************************************************************
 */

#include "adv_laser_draw.h"

#include "ir_sensor.h"
#include "line_follow.h"
#include "motor.h"
#include "oled.h"
#include "oled_text.h"
#include "servo.h"
#include "turn.h"

#include <stdio.h>

/* ========================== 激光引脚 ====================================== */

#define 激光_端口      GPIOB
#define 激光_引脚      GPIO_PIN_5

static void 激光_引脚初始化(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitStruct.Pin   = 激光_引脚;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(激光_端口, &GPIO_InitStruct);

    HAL_GPIO_WritePin(激光_端口, 激光_引脚, GPIO_PIN_RESET);
}

static void 激光_开(void)
{
    HAL_GPIO_WritePin(激光_端口, 激光_引脚, GPIO_PIN_SET);
}

static void 激光_关(void)
{
    HAL_GPIO_WritePin(激光_端口, 激光_引脚, GPIO_PIN_RESET);
}

/* ========================== 正弦查找表 ==================================== */
/*
 * 256 点正弦表：sin_table[i] ≈ round(127 * sin(2π * i / 256))
 * 值域 [-127, +127] 对应归一化 [-1.0, +1.0]
 * 步进 1 对应 360°/256 ≈ 1.406°
 */

static const int8_t sin_table[256] = {
      0,   3,   6,   9,  12,  15,  18,  21,  24,  27,  30,  33,  36,  39,
     42,  45,  48,  51,  54,  57,  59,  62,  65,  67,  70,  73,  75,  78,
     80,  82,  85,  87,  89,  91,  94,  96,  98, 100, 102, 103, 105, 107,
    108, 110, 112, 113, 114, 116, 117, 118, 119, 120, 121, 122, 123, 123,
    124, 125, 125, 126, 126, 126, 126, 126, 127, 126, 126, 126, 126, 126,
    125, 125, 124, 123, 123, 122, 121, 120, 119, 118, 117, 116, 114, 113,
    112, 110, 108, 107, 105, 103, 102, 100,  98,  96,  94,  91,  89,  87,
     85,  82,  80,  78,  75,  73,  70,  67,  65,  62,  59,  57,  54,  51,
     48,  45,  42,  39,  36,  33,  30,  27,  24,  21,  18,  15,  12,   9,
      6,   3,   0,  -3,  -6,  -9, -12, -15, -18, -21, -24, -27, -30, -33,
    -36, -39, -42, -45, -48, -51, -54, -57, -59, -62, -65, -67, -70, -73,
    -75, -78, -80, -82, -85, -87, -89, -91, -94, -96, -98,-100,-102,-103,
   -105,-107,-108,-110,-112,-113,-114,-116,-117,-118,-119,-120,-121,-122,
   -123,-123,-124,-125,-125,-126,-126,-126,-126,-126,-127,-126,-126,-126,
   -126,-126,-125,-125,-124,-123,-123,-122,-121,-120,-119,-118,-117,-116,
   -114,-113,-112,-110,-108,-107,-105,-103,-102,-100, -98, -96, -94, -91,
    -89, -87, -85, -82, -80, -78, -75, -73, -70, -67, -65, -62, -59, -57,
    -54, -51, -48, -45, -42, -39, -36, -33, -30, -27, -24, -21, -18, -15,
    -12,  -9,  -6,  -3,
};

static int16_t sin_lut(uint8_t a)   { return (int16_t)sin_table[a]; }
static int16_t cos_lut(uint8_t a)   { return sin_lut((uint8_t)(a + 64U)); }

/* ========================== 坐标映射 ====================================== */

#define 绘图_中心Pan    85
#define 绘图_跨度Pan    30
#define 绘图_中心Tilt   40
#define 绘图_跨度Tilt   20

/*
 * 归一化坐标范围：[-127, +127] 映射到 [-1.0, +1.0]
 * 绘图区域：[-102, +102]（保留 20% 边距，即 127 * 0.8 ≈ 102）
 */

/**
  * @brief  归一化坐标 (x,y) ∈ [-127,+127] → 舵机角度 → 激光打点
  */
static void 激光_画归一化点(int16_t x, int16_t y)
{
    int16_t pan_deg, tilt_deg;

    if (x < -102) x = -102;
    if (x >  102) x =  102;
    if (y < -102) y = -102;
    if (y >  102) y =  102;

    pan_deg  = (int16_t)(绘图_中心Pan  + (int32_t)x * 绘图_跨度Pan  / 102);
    tilt_deg = (int16_t)(绘图_中心Tilt + (int32_t)y * 绘图_跨度Tilt / 102);

    舵机_设置角度(舵机_水平, pan_deg);
    舵机_设置角度(舵机_垂直, tilt_deg);
    HAL_Delay(6);

    激光_开();
    HAL_Delay(12);
    激光_关();
}

/**
  * @brief  从 (x1,y1) 到 (x2,y2) 画直线段（取 steps 个中间点）
  */
static void 激光_画线段(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint8_t steps)
{
    uint8_t s;
    int16_t dx, dy, sx, sy;

    if (steps < 2U) steps = 2U;

    dx = x2 - x1;
    dy = y2 - y1;

    for (s = 0U; s <= steps; s++) {
        sx = x1 + (int16_t)((int32_t)dx * s / steps);
        sy = y1 + (int16_t)((int32_t)dy * s / steps);
        激光_画归一化点(sx, sy);
    }
}

/* ========================== 图形生成（纯整数） ============================= */

/**
  * @brief  画正三角形（等边，顶点朝上）
  */
static void 激光_画三角形(void)
{
#define TRI_R     83    /* 外接圆半径 ≈ 0.65 * 127 */
#define TRI_SQRT  72    /* TRI_R * √3/2 ≈ 72 */

    /* 三个顶点：上顶点 (0, TRI_R)，左下 (-TRI_SQRT, -TRI_R/2)，右下对称 */
    const int16_t vertices[4][2] = {
        {       0,        TRI_R },
        { -TRI_SQRT, -(TRI_R / 2) },
        {  TRI_SQRT, -(TRI_R / 2) },
        {       0,        TRI_R },   /* 闭合 */
    };

    激光_画归一化点(vertices[0][0], vertices[0][1]);
    激光_画线段(vertices[0][0], vertices[0][1],
                vertices[1][0], vertices[1][1], 20U);
    激光_画线段(vertices[1][0], vertices[1][1],
                vertices[2][0], vertices[2][1], 20U);
    激光_画线段(vertices[2][0], vertices[2][1],
                vertices[3][0], vertices[3][1], 20U);

#undef TRI_R
#undef TRI_SQRT
}

/**
  * @brief  画正方形
  */
static void 激光_画正方形(void)
{
#define SQ_R  83    /* 半边长 ≈ 0.65 * 127 */

    const int16_t v[5][2] = {
        { -SQ_R, -SQ_R }, {  SQ_R, -SQ_R }, {  SQ_R,  SQ_R },
        { -SQ_R,  SQ_R }, { -SQ_R, -SQ_R }
    };

    激光_画线段(v[0][0], v[0][1], v[1][0], v[1][1], 22U);
    激光_画线段(v[1][0], v[1][1], v[2][0], v[2][1], 22U);
    激光_画线段(v[2][0], v[2][1], v[3][0], v[3][1], 22U);
    激光_画线段(v[3][0], v[3][1], v[4][0], v[4][1], 22U);

#undef SQ_R
}

/**
  * @brief  画圆形（36 等分，查表 sin/cos）
  */
static void 激光_画圆(void)
{
#define CIRC_R  83    /* 半径 */
    uint8_t i;

    for (i = 0U; i < 36U; i++) {
        uint8_t a = (uint8_t)(i * 256U / 36U);
        int16_t x = (int16_t)((int32_t)cos_lut(a) * CIRC_R / 127);
        int16_t y = (int16_t)((int32_t)sin_lut(a) * CIRC_R / 127);
        激光_画归一化点(x, y);
    }

#undef CIRC_R
}

/**
  * @brief  画正弦波：3 个周期，x 扫满绘图区
  *
  *  归一化空间：x ∈ [-95, +95]（约 3/4 绘图宽度）
  *              y = 42 * sin(3 * 2π * 相位)  振幅 42 ≈ 4cm
  */
static void 激光_画正弦波(void)
{
#define WAVE_XR    95
#define WAVE_AMP   42
    uint8_t steps = 100U;
    uint8_t i;

    for (i = 0U; i <= steps; i++) {
        int16_t x = (int16_t)(-WAVE_XR + (int32_t)(2 * WAVE_XR) * i / steps);

        /* 相位：3 个周期跨越整个 x 范围 (-WAVE_XR → +WAVE_XR)
           LUT 索引 = (3 * 256 * (x + WAVE_XR) / (2 * WAVE_XR)) % 256 */
        uint32_t phase = (uint32_t)(3U * 256U * (uint32_t)(x + WAVE_XR) / (uint32_t)(2 * WAVE_XR));
        uint8_t  idx   = (uint8_t)(phase % 256U);
        int16_t  y    = (int16_t)((int32_t)WAVE_AMP * sin_lut(idx) / 127);

        激光_画归一化点(x, y);
    }

#undef WAVE_XR
#undef WAVE_AMP
}

/**
  * @brief  根据发车点画对应图形
  */
static void 激光_画图形(char 发车点)
{
    OLED_清屏();
    OLED_显示中文串(0, 0, "激光绘图");

    switch (发车点) {
    case 'A':
        OLED_显示中文串(0, 16, "正三角形");
        OLED_刷新();
        激光_画三角形();
        break;
    case 'B':
        OLED_显示中文串(0, 16, "正方形");
        OLED_刷新();
        激光_画正方形();
        break;
    case 'C':
        OLED_显示中文串(0, 16, "圆形");
        OLED_刷新();
        激光_画圆();
        break;
    case 'D':
        OLED_显示中文串(0, 16, "正弦波");
        OLED_刷新();
        激光_画正弦波();
        break;
    default:
        break;
    }
}

/* ========================== 任务入口 ====================================== */

void 激光绘图(char 发车点)
{
    char 行[24];

    激光_引脚初始化();
    激光_关();

    OLED_清屏();
    (void)snprintf(行, sizeof(行), "发车点 %c", 发车点);
    OLED_显示中文串(0, 0, 行);
    OLED_刷新();
    HAL_Delay(800);

    /* ---- ① 巡线到 AB 中段（第一个拐角处当近似中点）---- */
    巡线(40);
    电机_全部停止();
    HAL_Delay(300);

    /* ---- ② 云台就位 + 画图 ---- */
    舵机_全部中位();
    HAL_Delay(200);

    激光_画图形(发车点);

    /* ---- ③ 云台归位 + 继续绕场一圈 ---- */
    舵机_全部中位();
    激光_关();

    OLED_清屏();
    OLED_显示中文串(0, 16, "绕场一周");
    OLED_刷新();
    HAL_Delay(500);

    右转();
    巡线(40);
    电机_全部停止();
    HAL_Delay(200);
    右转();
    巡线(40);
    电机_全部停止();
    HAL_Delay(200);
    右转();
    巡线(40);
    电机_全部停止();

    OLED_清屏();
    OLED_显示中文串(0, 16, "绘图完成");
    OLED_刷新();
    HAL_Delay(1500);
}