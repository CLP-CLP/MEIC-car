/**
 ******************************************************************************
 * @file    gray_calib.c
 * @brief   灰度（红外巡线）传感器自动校准模块
 *
 *  触发：连续上电 3 次（每次 2 秒内断电）-> 第 3 次进入自动扫线校准
 *
 *  校准流程（约 7 秒，全自动）
 *      1. 先把上电次数清零写回 Flash（中途断电也不会反复进校准）
 *      2. LED 快闪 5 次后常亮；电机按「转-停-反向转-停」摆动扫过黑线
 *      3. 过程中约 1 kHz 连续采样，每路记录最小值(白底)和最大值(黑线)
 *      4. 按净转角反向转回原来的朝向（自动回正）
 *      5. 只统计真正扫到黑线的那几路，算出中/强阈值，写回 Flash
 *
 *  Flash 记录布局（12 个半字，在 灰度_闪存页地址 那一页开头）
 *      [0]  魔数 0xA5C4
 *      [1]  上电次数
 *      [2]  标志：1 = 有有效校准数据
 *      [3]  中阈值
 *      [4]  强阈值
 *      [5]  保留
 *      [6]  白底平均（记录用）
 *      [7]  黑线平均（记录用）
 *      [8..10] 保留
 *      [11] 校验 = [0..10] 之和
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "gray_calib.h"
#include "board.h"          /* LED_ON / LED_OFF：校准全程用板载 LED 给信号 */
#include "button.h"         /* 手动校准：确认键采点、长按取消 */
#include "ir_sensor.h"
#include "motor.h"
#include "oled.h"           /* 手动校准：屏幕提示 */
#include "oled_text.h"      /* 界面文案集中表 */

#include <stdio.h>
#include <string.h>

/* ============================== 记录布局 =================================== */

#define 灰度_记录半字数         19U

#define 记录位_魔数             0U
#define 记录位_上电次数         1U
#define 记录位_标志             2U
#define 记录位_阈值首           3U    /* 灰度_各路阈值[0..4]   -> 3..7 */
#define 记录位_强阈值首         8U    /* 灰度_各路强阈值[0..4] -> 8..12 */
#define 记录位_左电机速度       13U
#define 记录位_右电机速度       14U
#define 记录位_最低PWM          15U
#define 记录位_车库40毫秒       16U   /* 40 cm 车库跑出来的时间，见 main.c 的车库时间标定 */
#define 记录位_车库25毫秒       17U   /* 25 cm 车库跑出来的时间 */
#define 记录位_校验             18U

#define 记录标志_已校准         1U

/* ============================== 校准结果 =================================== */

uint16_t 灰度_各路阈值[巡线_路数];
uint16_t 灰度_各路强阈值[巡线_路数];

/** 电机可保存设置，上电时从 Flash 加载 */
int16_t 灰度_左电机速度 = 灰度_默认左电机速度;
int16_t 灰度_右电机速度 = 灰度_默认右电机速度;
uint8_t 灰度_最低PWM    = 灰度_默认最低PWM;

/** 把所有路的阈值设成默认值 */
static void 灰度_用默认阈值(void)
{
    uint8_t i;

    for (i = 0U; i < 巡线_路数; i++) {
        灰度_各路阈值[i]   = 灰度_默认各路阈值;
        灰度_各路强阈值[i] = 灰度_默认各路强阈值;
    }
}

/**
  * @brief  原始值 -> 黑度千分（0~1000），给屏幕做连续显示用
  * @note   公式和 line_follow.c 的 巡线_黑度() 完全一致，只是走整数：
  *             跨度 = 4 × (强阈值 - 阈值)     白底 = 阈值 - 跨度 / 2
  *             黑度 = (原始值 - 白底) × 1000 / 跨度
  *         所以 白底 = 0、阈值 = 500、黑线 = 1000，显示和判据天然对齐。
  */
uint16_t 灰度_黑度千分(uint8_t 第几路, uint16_t 原始值)
{
    int32_t 阈值;
    int32_t 强阈值;
    int32_t 跨度;
    int32_t 白底;
    int32_t 黑度;

    if (第几路 >= 巡线_路数) {
        return 0U;              /* 序号越界，安全返回 0（显示为最淡） */
    }

    阈值   = (int32_t)灰度_各路阈值[第几路];
    强阈值 = (int32_t)灰度_各路强阈值[第几路];
    跨度   = 4 * (强阈值 - 阈值);

    /* 没校准 / 校准异常时给个兜底跨度，避免除零和数值乱跳 */
    if (跨度 < (int32_t)灰度_最小标定跨度) {
        跨度 = (int32_t)灰度_最小标定跨度;
    }

    白底 = 阈值 - 跨度 / 2;

    黑度 = (((int32_t)原始值 - 白底) * 1000) / 跨度;

    if (黑度 < 0)    { 黑度 = 0; }
    if (黑度 > 1000) { 黑度 = 1000; }

    return (uint16_t)黑度;
}

/** 1 = 阈值来自 Flash 的校准数据 */
static uint8_t 已校准 = 0U;

/** 1 = 正在擦写 Flash（防止 SysTick 里的窗口检查插进来抢 Flash） */
static volatile uint8_t 正在写Flash = 0U;

/** 1 = 正在校准流程中 */
static volatile uint8_t 校准进行中 = 0U;

/** 1 = 上电窗口已经处理过（整个上电周期只处理一次） */
static volatile uint8_t 窗口已处理 = 0U;

/** 上电时记下的次数，供 灰度_就绪后处理() 打印 */
static uint16_t 上次次数 = 0U;

/** 1 = 本次上电需要做扫线校准（等串口就绪后再跑） */
static volatile uint8_t 待校准 = 0U;

/* ============================== 私有函数声明 =============================== */
static void     灰度_读记录(uint16_t *记录);
static uint8_t  灰度_写记录(const uint16_t *记录);
static void     灰度_清上电次数(void);
static uint16_t 灰度_算校验(const uint16_t *记录);
static void     灰度_闪灯(uint16_t 次数, uint16_t 亮毫秒, uint16_t 灭毫秒);

/* ============================== Flash 读写 ================================= */

static void 灰度_读记录(uint16_t *记录)
{
    const uint16_t *页 = (const uint16_t *)灰度_闪存页地址;
    uint16_t i;

    for (i = 0U; i < 灰度_记录半字数; i++) {
        记录[i] = 页[i];
    }
}

static uint8_t 灰度_写记录(const uint16_t *记录)
{
    FLASH_EraseInitTypeDef 擦除 = {0};
    uint32_t 出错页 = 0U;
    uint16_t i;

    正在写Flash = 1U;                   /* 让 SysTick 里的窗口检查避开 */

    if (HAL_FLASH_Unlock() != HAL_OK) {
        正在写Flash = 0U;
        return 0U;
    }

    擦除.TypeErase   = FLASH_TYPEERASE_PAGES;
    擦除.PageAddress = 灰度_闪存页地址;
    擦除.NbPages     = 1U;

    if (HAL_FLASHEx_Erase(&擦除, &出错页) != HAL_OK) {
        (void)HAL_FLASH_Lock();
        正在写Flash = 0U;
        return 0U;
    }

    for (i = 0U; i < 灰度_记录半字数; i++)
    {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                              灰度_闪存页地址 + (uint32_t)i * 2U,
                              (uint64_t)记录[i]) != HAL_OK)
        {
            (void)HAL_FLASH_Lock();
            正在写Flash = 0U;
            return 0U;
        }
    }

    (void)HAL_FLASH_Lock();
    正在写Flash = 0U;
    return 1U;
}

/**
  * @brief  把 Flash 里的上电次数清零（已经是 0 就不写，省 Flash 寿命）
  */
static void 灰度_清上电次数(void)
{
    uint16_t 记录[灰度_记录半字数];

    灰度_读记录(记录);

    if (记录[记录位_魔数] != 灰度_魔数) {
        return;
    }

    if (记录[记录位_上电次数] == 0U) {
        return;
    }

    记录[记录位_上电次数] = 0U;
    记录[记录位_校验]     = 灰度_算校验(记录);
    (void)灰度_写记录(记录);
}

/**
  * @brief  把当前的阈值/电机设置铺成一条完整记录（魔数、校验都填好）
  * @note   用于"Flash 里还没有有效记录，但用户要先存车库时间"的场合：
  *         这样不会因为记录不存在就写不进东西，也不会写进去一条垃圾。
  */
static void 灰度_铺默认记录(uint16_t *记录)
{
    uint16_t i;

    memset(记录, 0, sizeof(uint16_t) * 灰度_记录半字数);

    记录[记录位_魔数] = 灰度_魔数;

    for (i = 0U; i < 巡线_路数; i++)
    {
        记录[记录位_阈值首 + i]   = 灰度_默认各路阈值;
        记录[记录位_强阈值首 + i] = 灰度_默认各路强阈值;
    }

    记录[记录位_左电机速度] = (uint16_t)灰度_默认左电机速度;
    记录[记录位_右电机速度] = (uint16_t)灰度_默认右电机速度;
    记录[记录位_最低PWM]    = 灰度_默认最低PWM;
}

static uint16_t 灰度_算校验(const uint16_t *记录)
{
    uint16_t 和 = 0U;
    uint16_t i;

    for (i = 0U; i < 灰度_记录半字数; i++)
    {
        if (i != 记录位_校验) {
            和 = (uint16_t)(和 + 记录[i]);
        }
    }

    return 和;
}

/**
  * @brief  闪 LED：亮 亮毫秒、灭 灭毫秒，重复 次数 次
  */
static void 灰度_闪灯(uint16_t 次数, uint16_t 亮毫秒, uint16_t 灭毫秒)
{
    uint16_t i;

    for (i = 0U; i < 次数; i++)
    {
        LED_ON();
        HAL_Delay(亮毫秒);
        LED_OFF();
        HAL_Delay(灭毫秒);
    }
}

/* ============================== 扫线校准 =================================== */

void 灰度_校准(void)
{
    uint16_t 记录[灰度_记录半字数];
    uint16_t 最小[巡线_路数];
    uint16_t 最大[巡线_路数];
    巡线_读数 读数;
    uint32_t 开始时刻;
    uint32_t 状态时刻;
    uint32_t 白和 = 0U;
    uint32_t 黑和 = 0U;
    uint32_t 采样数 = 0U;
    uint16_t 白平均;
    uint16_t 黑平均;
    uint16_t 跨度_i;
    uint16_t 有效路数 = 0U;
    uint16_t 本状态时长;
    uint8_t  状态 = 0U;                     /* 0=向左转 1=停 2=向右转 3=停 */
    uint16_t 摆动次数 = 1U;                 /* 已经摆动的次数（转一次算一次） */
    int32_t  净转角 = 0;                    /* 净侧过的时间(ms)，正 = 净向右 */
    int8_t   转向前 = 0;                    /* 当前转动方向：-1 左, +1 右, 0 停 */
    uint16_t i;

    校准进行中 = 1U;
    灰度_读记录(记录);

    /* ---- 关键：进来第一件事就把上电次数清零并写回 ----
       否则校准中途断电时，Flash 里的次数会一直停在 3，
       导致之后每次上电都又进校准。 */
    记录[记录位_上电次数] = 0U;
    记录[记录位_校验]     = 灰度_算校验(记录);
    (void)灰度_写记录(记录);

    /* ---- 进场信号：快闪 5 次，之后 LED 一直亮着 ---- */
    灰度_闪灯(5U, 80U, 80U);
    LED_ON();

    printf("\r\n");
    printf("======= gray auto-calibration (sweep) =======\r\n");
    printf("the car sweeps left/right across the black line for %u s\r\n",
           (unsigned)(灰度_扫线毫秒 / 1000U));

    if (灰度_扫线速度 == 0) {
        printf("(motors are NOT driven here: rock the car by hand)\r\n");
    }

    /* ---- 初始化每路极值 ---- */
    for (i = 0U; i < 巡线_路数; i++)
    {
        最小[i] = 0xFFFFU;
        最大[i] = 0U;
    }

    /* ---- 扫线并连续采样：转 -> 停 -> 反向转 -> 停 循环 ---- */
    开始时刻 = HAL_GetTick();
    状态时刻 = 开始时刻;

    if (灰度_扫线速度 != 0) {
        电机_设置双轮(-灰度_扫线速度, 灰度_扫线速度);      /* 先向左转 */
        转向前 = -1;
    }

    while ((uint32_t)(HAL_GetTick() - 开始时刻) < 灰度_扫线毫秒)
    {
        if (灰度_扫线速度 != 0)
        {
            /* 转动状态用 旋转毫秒，停顿状态用 停留毫秒 */
            本状态时长 = ((状态 == 0U) || (状态 == 2U))
                       ? 灰度_扫线旋转毫秒 : 灰度_扫线停留毫秒;

#if 灰度_首次半角度
            /* 车一开始就压在黑线上：第一次只转半个角度出去，
               这样整段扫描以黑线为中心左右对称，不会一上来就甩过头 */
            if ((状态 == 0U) && (摆动次数 == 1U)) {
                本状态时长 = (uint16_t)(灰度_扫线旋转毫秒 / 2U);
            }
#endif

            if ((uint32_t)(HAL_GetTick() - 状态时刻) >= 本状态时长)
            {
                /* 先把这个状态"转过的时间"按方向累加，正 = 右，负 = 左 */
                净转角 += (int32_t)(HAL_GetTick() - 状态时刻) * (int32_t)转向前;

                状态时刻 = HAL_GetTick();
                状态 = (uint8_t)((状态 + 1U) & 3U);

                switch (状态)
                {
                    case 0U:                                /* 向左转 */
                        电机_设置双轮(-灰度_扫线速度, 灰度_扫线速度);
                        摆动次数++;
                        转向前 = -1;
                        break;

                    case 2U:                                /* 向右转 */
                        电机_设置双轮(灰度_扫线速度, -灰度_扫线速度);
                        摆动次数++;
                        转向前 = 1;
                        break;

                    default:                                /* 1、3：停顿 */
                        电机_全部停止();
                        转向前 = 0;
                        break;
                }
            }
        }

        /* 采一次，更新每路的极值 */
        巡线_读取全部(&读数);

        for (i = 0U; i < 巡线_路数; i++)
        {
            if (读数.原始值[i] < 最小[i]) {
                最小[i] = 读数.原始值[i];
            }
            if (读数.原始值[i] > 最大[i]) {
                最大[i] = 读数.原始值[i];
            }
        }

        采样数++;
        HAL_Delay(1U);
    }

    /* 最后一段（循环结束时正在转的那一段）也要算进净转角 */
    净转角 += (int32_t)(HAL_GetTick() - 状态时刻) * (int32_t)转向前;
    转向前 = 0;

    电机_全部停止();                        /* 扫完立刻停住 */

#if 灰度_校准后回正
    /* ---- 自动回正 ----
       扫线时左右转过的时间不相等（第一次还是半角度），结束时车头是歪的；
       这里按净转角反向转回来。没有编码器，用时间估算角度，属近似回正。 */
    if ((灰度_扫线速度 != 0) && (净转角 != 0))
    {
        uint32_t 回正毫秒;

        if (净转角 > 0) {
            回正毫秒 = (uint32_t)净转角 * 灰度_回正比例 / 100U;
        } else {
            回正毫秒 = (uint32_t)(-净转角) * 灰度_回正比例 / 100U;
        }

        printf("re-center: net %ld ms -> turn back %lu ms\r\n",
               (long)净转角, (unsigned long)回正毫秒);

        if (净转角 > 0) {
            电机_设置双轮(-灰度_扫线速度, 灰度_扫线速度);   /* 净向右偏 -> 向左转回 */
        } else {
            电机_设置双轮(灰度_扫线速度, -灰度_扫线速度);   /* 净向左偏 -> 向右转回 */
        }

        HAL_Delay(回正毫秒);
        电机_全部停止();
    }
#endif

    LED_OFF();

    /* ---- 统计：只认真正扫到黑线的那几路 ---- */
    printf("samples %lu, sweeps %u, per-channel min/max:\r\n",
           (unsigned long)采样数, 摆动次数);
    printf("  ch   min(white)  max(black)   span\r\n");

    for (i = 0U; i < 巡线_路数; i++)
    {
        跨度_i = (uint16_t)(最大[i] - 最小[i]);

        printf("  %u   %5u    %5u    %5u %s\r\n",
               i, 最小[i], 最大[i], 跨度_i,
               (跨度_i >= 灰度_最小有效跨度) ? "" : "<- no line seen, ignored");

        if (跨度_i >= 灰度_最小有效跨度)
        {
            有效路数++;
            白和 += 最小[i];
            黑和 += 最大[i];
        }
    }

    if (有效路数 < 灰度_最少有效路数)
    {
        printf("!!! only %u of %u channels saw the black line: calibration FAILED, "
               "keeping the old thresholds\r\n",
               有效路数, (unsigned)灰度_最少有效路数);
        printf("!!! put the car near the black line, then power-cycle 3 times quickly\r\n");

        灰度_闪灯(30U, 80U, 80U);           /* 失败信号：急促连闪 30 次 */
        校准进行中 = 0U;
        return;
    }

    /* ---- 算每路阈值 ---- */
    for (i = 0U; i < 巡线_路数; i++)
    {
        跨度_i = (uint16_t)(最大[i] - 最小[i]);

        if (跨度_i >= 灰度_最小有效跨度)
        {
            灰度_各路阈值[i]   = (uint16_t)(((uint32_t)最小[i] + (uint32_t)最大[i]) / 2U);
            灰度_各路强阈值[i] = (uint16_t)((uint32_t)最小[i]
                              + (uint32_t)跨度_i * 3U / 4U);
        }
        /* 跨度不够的那一路保持默认值 */
    }

    白平均 = (uint16_t)(白和 / 有效路数);
    黑平均 = (uint16_t)(黑和 / 有效路数);

    printf("valid channels %u, white avg %u, black avg %u\r\n", 有效路数, 白平均, 黑平均);
    for (i = 0U; i < 巡线_路数; i++) {
        printf("  ch%u: on=%u  strong=%u\r\n", i, 灰度_各路阈值[i], 灰度_各路强阈值[i]);
    }

    /* ---- 保存 ---- */
    记录[记录位_标志]     = 记录标志_已校准;
    for (i = 0U; i < 巡线_路数; i++) {
        记录[记录位_阈值首 + i]   = 灰度_各路阈值[i];
        记录[记录位_强阈值首 + i] = 灰度_各路强阈值[i];
    }
    记录[记录位_上电次数] = 0U;
    记录[记录位_校验]     = 灰度_算校验(记录);

    if (灰度_写记录(记录) != 0U)
    {
        已校准 = 1U;
        printf("saved to Flash; every boot from now on reuses it\r\n");

        LED_ON();
        HAL_Delay(2000U);                   /* 成功信号：长亮 2 秒 */
        LED_OFF();
        灰度_闪灯(3U, 500U, 500U);          /* 再慢闪 3 次 */
    }
    else
    {
        printf("!!! Flash write failed: this calibration is valid for this run only\r\n");
        灰度_闪灯(30U, 80U, 80U);
    }

    printf("============================================\r\n\r\n");
    校准进行中 = 0U;
}

/* ============================== 手动两点校准 =============================== */

/** 采一组五路平均值：先让车停稳，再连采 灰度_手动采样次数 次求平均 */
static void 灰度_采平均(uint16_t *结果)
{
    uint32_t 累加[巡线_路数];
    uint16_t i;
    uint16_t k;

    /* 刚按完确认键，手还搭在车上、车可能还在晃，先等它稳下来。
       这 300 ms 也是给用户看一眼"正在采样"的机会。 */
    OLED_显示中文串(0, 32, 文本_采样中);
    OLED_刷新();
    HAL_Delay(300U);

    for (i = 0U; i < 巡线_路数; i++) {
        累加[i] = 0U;
    }

    for (k = 0U; k < 灰度_手动采样次数; k++)
    {
        for (i = 0U; i < 巡线_路数; i++) {
            累加[i] += (uint32_t)巡线_读取第几路(i);
        }
        HAL_Delay(灰度_手动采样间隔毫秒);
    }

    for (i = 0U; i < 巡线_路数; i++) {
        结果[i] = (uint16_t)(累加[i] / 灰度_手动采样次数);
    }
}

/** 画一屏四行提示（第一行是标题，最后一行固定是"长按=取消"）
    手动校准 / 车库时间标定共用 */
void 灰度_提示屏(const char *标题, const char *第一行, const char *第二行)
{
    OLED_清屏();
    OLED_显示中文串(0, 0,  标题);
    OLED_显示中文串(0, 16, 第一行);
    OLED_显示中文串(0, 32, 第二行);
    OLED_显示中文串(0, 48, 文本_长按等于取消);
    OLED_刷新();
}

/**
  * @brief  等一次按键（手动校准 / 车库时间标定共用）
  * @retval 1 = 按了确认短按；0 = 长按确认（放弃）
  * @note   按键状态机要求被反复轮询（消抖 20 ms、长按 2 s 都靠它自己计时），
  *         所以这里 1 ms 转一圈，不要用一大段 HAL_Delay 把轮询掐断。
  */
uint8_t 灰度_等确认(void)
{
    for (;;)
    {
        btn_event_t 事件 = 按键_取事件();

        if (事件 == BTN_SEL) {
            return 1U;
        }
        if (事件 == BTN_BACK) {
            return 0U;
        }

        HAL_Delay(1U);
    }
}

void 灰度_手动校准(void)
{
    uint16_t 记录[灰度_记录半字数];
    uint16_t 黑值[巡线_路数];
    uint16_t 白值[巡线_路数];
    uint16_t 阈值[巡线_路数];
    uint16_t 强阈值[巡线_路数];
    uint16_t 有效路数 = 0U;
    uint16_t i;

    校准进行中 = 1U;
    灰度_读记录(记录);

    /* 进屋先清上电次数并写回：万一在校准中途断电，Flash 里的次数不会卡在 3，
       否则之后每次上电都会又跳进校准。这一步和自动扫线一致。 */
    记录[记录位_上电次数] = 0U;
    记录[记录位_校验]     = 灰度_算校验(记录);
    (void)灰度_写记录(记录);

    灰度_闪灯(2U, 80U, 80U);           /* 进场信号：连闪 2 次 */
    LED_ON();

    printf("\r\n===== gray manual 2-point calibration =====\r\n");
    printf("put the car on the BLACK line, press SEL; then on WHITE, press SEL\r\n");

    /* ---- 第 1 步：黑线 ---- */
    灰度_提示屏(文本_手动校准1, 文本_车横放线穿五路, 文本_确认等于采样);

    if (灰度_等确认() == 0U) {
        printf("manual calibration cancelled (black step)\r\n");
        OLED_清屏();
        OLED_显示中文串(0, 0, 文本_已取消);
        OLED_刷新();
        LED_OFF();
        HAL_Delay(800U);
        校准进行中 = 0U;
        return;
    }

    灰度_采平均(黑值);

    printf("black: %u %u %u %u %u\r\n",
           (unsigned)黑值[0], (unsigned)黑值[1], (unsigned)黑值[2],
           (unsigned)黑值[3], (unsigned)黑值[4]);

    /* ---- 第 2 步：白底 ---- */
    灰度_提示屏(文本_手动校准2, 文本_整机放到白底上, 文本_确认等于采样);

    if (灰度_等确认() == 0U) {
        printf("manual calibration cancelled (white step)\r\n");
        OLED_清屏();
        OLED_显示中文串(0, 0, 文本_已取消);
        OLED_刷新();
        LED_OFF();
        HAL_Delay(800U);
        校准进行中 = 0U;
        return;
    }

    灰度_采平均(白值);

    printf("white: %u %u %u %u %u\r\n",
           (unsigned)白值[0], (unsigned)白值[1], (unsigned)白值[2],
           (unsigned)白值[3], (unsigned)白值[4]);

    /* ---- 算每路阈值：跨度不够的那一路保持原值 ---- */
    for (i = 0U; i < 巡线_路数; i++)
    {
        int32_t 跨度 = (int32_t)黑值[i] - (int32_t)白值[i];

        printf("  ch%u: white=%u black=%u span=%ld %s\r\n",
               (unsigned)i, (unsigned)白值[i], (unsigned)黑值[i], (long)跨度,
               (跨度 >= (int32_t)灰度_最小有效跨度) ? "" : "<- too small, kept");

        if (跨度 >= (int32_t)灰度_最小有效跨度)
        {
            有效路数++;
            阈值[i]   = (uint16_t)(((uint32_t)白值[i] + (uint32_t)黑值[i]) / 2U);
            强阈值[i] = (uint16_t)((uint32_t)白值[i] + (uint32_t)跨度 * 3U / 4U);
        }
        else
        {
            阈值[i]   = 灰度_各路阈值[i];
            强阈值[i] = 灰度_各路强阈值[i];
        }
    }

    LED_OFF();

    /* ---- 有效路数不够：整次作废，一个字节都不写 ---- */
    if (有效路数 < 灰度_最少有效路数)
    {
        char 行[20];

        printf("!!! only %u valid channel(s): calibration FAILED, Flash untouched\r\n",
               (unsigned)有效路数);

        OLED_清屏();
        OLED_显示中文串(0, 0, 文本_校准失败);
        (void)snprintf(行, sizeof(行), "ok %u/%u", (unsigned)有效路数, (unsigned)巡线_路数);
        OLED_显示ASCII串(0, 16, 行);
        OLED_显示中文串(0, 32, 文本_没压准);
        OLED_刷新();

        灰度_闪灯(30U, 80U, 80U);       /* 失败信号：急促连闪 30 次 */
        HAL_Delay(1200U);
        校准进行中 = 0U;
        return;
    }

    /* ---- 落盘 ---- */
    for (i = 0U; i < 巡线_路数; i++)
    {
        灰度_各路阈值[i]   = 阈值[i];
        灰度_各路强阈值[i] = 强阈值[i];

        记录[记录位_阈值首 + i]   = 阈值[i];
        记录[记录位_强阈值首 + i] = 强阈值[i];
    }

    记录[记录位_标志]     = 记录标志_已校准;
    记录[记录位_上电次数] = 0U;
    记录[记录位_校验]     = 灰度_算校验(记录);

    if (灰度_写记录(记录) != 0U)
    {
        已校准 = 1U;
        printf("saved to Flash; every boot from now on reuses it\r\n");
    }
    else
    {
        printf("!!! Flash write failed: this calibration is valid for this run only\r\n");
    }

    /* ---- 结果屏：每路一行，中阈值/强阈值 ---- */
    {
        char 行[20];

        OLED_清屏();
        OLED_显示中文串(0, 0, 文本_校准完成);

        for (i = 0U; i < 巡线_路数; i++)
        {
            (void)snprintf(行, sizeof(行), "%u:%u/%u",
                           (unsigned)i, (unsigned)阈值[i], (unsigned)强阈值[i]);
            OLED_显示ASCII串(0, (int16_t)(16 + i * 8), 行);
        }
        OLED_刷新();
    }

    LED_ON();
    HAL_Delay(600U);
    LED_OFF();
    HAL_Delay(1600U);                    /* 让结果屏停留一会儿 */

    printf("============================================\r\n\r\n");
    校准进行中 = 0U;
}

/* ============================== 对外接口 =================================== */

/* ---------------------------- 车库时间 ------------------------------------- */

uint8_t 灰度_取车库时间(uint16_t *时间40毫秒, uint16_t *时间25毫秒)
{
    uint16_t 记录[灰度_记录半字数];

    灰度_读记录(记录);

    if ((记录[记录位_魔数] != 灰度_魔数)
        || (记录[记录位_校验] != 灰度_算校验(记录)))
    {
        return 0U;                      /* 整条记录都不对，谈不上有标定值 */
    }

    if ((记录[记录位_车库40毫秒] == 0U) || (记录[记录位_车库25毫秒] == 0U)) {
        return 0U;                      /* 记录有效，但还没做过车库标定 */
    }

    if (时间40毫秒 != NULL) { *时间40毫秒 = 记录[记录位_车库40毫秒]; }
    if (时间25毫秒 != NULL) { *时间25毫秒 = 记录[记录位_车库25毫秒]; }

    return 1U;
}

uint8_t 灰度_存车库时间(uint16_t 时间40毫秒, uint16_t 时间25毫秒)
{
    uint16_t 记录[灰度_记录半字数];

    灰度_读记录(记录);

    /* 整条记录坏掉（比如刚烧录、还没校准过）时，先按默认值铺一条，
       否则这次标定就白跑了 —— 阈值那部分仍是默认值，用户照样可以再校准 */
    if ((记录[记录位_魔数] != 灰度_魔数)
        || (记录[记录位_校验] != 灰度_算校验(记录)))
    {
        printf("no valid record yet: laying down defaults before saving garage time\r\n");
        灰度_铺默认记录(记录);
    }

    记录[记录位_车库40毫秒] = 时间40毫秒;
    记录[记录位_车库25毫秒] = 时间25毫秒;
    记录[记录位_校验]       = 灰度_算校验(记录);

    if (灰度_写记录(记录) != 0U)
    {
        printf("garage time saved: 40cm=%u ms, 25cm=%u ms\r\n",
               (unsigned)时间40毫秒, (unsigned)时间25毫秒);
        return 1U;
    }

    printf("!!! Flash write failed: garage time is valid for this run only\r\n");
    return 0U;
}

void 灰度_上电检查(void)
{
    uint16_t 记录[灰度_记录半字数];
    uint16_t 次数;

    灰度_用默认阈值();      /* 先填默认值，读到有效校准数据再覆盖 */
    窗口已处理 = 0U;
    灰度_读记录(记录);

    if ((记录[记录位_魔数] != 灰度_魔数)
        || (记录[记录位_校验] != 灰度_算校验(记录)))
    {
        /* Flash 里没有有效记录（首次烧录后的第一次上电会走到这里） */
        memset(记录, 0, sizeof(记录));
        记录[记录位_魔数]     = 灰度_魔数;
        记录[记录位_上电次数] = 1U;
        记录[记录位_标志]     = 0U;
        记录[记录位_校验]     = 灰度_算校验(记录);

        (void)灰度_写记录(记录);
        次数 = 1U;
    }
    else
    {
        /* 先加载上次的校准结果，保证本次上电立刻可用 */
        if (记录[记录位_标志] == 记录标志_已校准)
        {
            uint8_t i;

            for (i = 0U; i < 巡线_路数; i++) {
                灰度_各路阈值[i]   = 记录[记录位_阈值首 + i];
                灰度_各路强阈值[i] = 记录[记录位_强阈值首 + i];
            }
            已校准 = 1U;
        }

        /* 加载电机设置 */
        灰度_左电机速度 = (int16_t)记录[记录位_左电机速度];
        灰度_右电机速度 = (int16_t)记录[记录位_右电机速度];
        灰度_最低PWM    = (uint8_t)记录[记录位_最低PWM];
        if (灰度_最低PWM > 100U) {
            灰度_最低PWM = 灰度_默认最低PWM;
        }
        if (灰度_左电机速度 < 0 || 灰度_左电机速度 > 100) {
            灰度_左电机速度 = 灰度_默认左电机速度;
        }
        if (灰度_右电机速度 < 0 || 灰度_右电机速度 > 100) {
            灰度_右电机速度 = 灰度_默认右电机速度;
        }
        电机_设置最小占空比(灰度_最低PWM);

        次数 = (uint16_t)(记录[记录位_上电次数] + 1U);
        记录[记录位_上电次数] = 次数;
        记录[记录位_校验]     = 灰度_算校验(记录);

        (void)灰度_写记录(记录);
    }

    /* 这里只记状态，不打印、不校准：
       打印和校准都必须等 USB 串口初始化完成之后再做 */

    if (次数 == 灰度_触发上电次数)
    {
        待校准 = 1U;
    }
    else if (次数 > 灰度_触发上电次数)
    {
        /* 遗留的异常计数：直接归零，防止"每次上电都进校准" */
        记录[记录位_上电次数] = 0U;
        记录[记录位_校验]     = 灰度_算校验(记录);
        (void)灰度_写记录(记录);
    }

    上次次数 = 次数;
}

/**
  * @brief  串口就绪之后调用：打印上电次数与阈值，并在需要时执行扫线校准
  */
void 灰度_就绪后处理(void)
{
    if (已校准 != 0U) {
        printf("gray thresholds (from Flash):\r\n");
    } else {
        printf("gray thresholds (default):\r\n");
    }
    for (uint8_t i = 0U; i < 巡线_路数; i++) {
        printf("  ch%u: on=%u  strong=%u\r\n", i, 灰度_各路阈值[i], 灰度_各路强阈值[i]);
    }

    printf("power-ups %u / %u (power must be cut within %u ms to count)\r\n",
           上次次数, (unsigned)灰度_触发上电次数, (unsigned)灰度_上电窗口毫秒);

    if (待校准 != 0U)
    {
        待校准 = 0U;
        灰度_校准();
    }
}

void 灰度_窗口检查(void)
{
    /* 由 SysTick 每 1 ms 调用，只在上电后第一次跨过窗口时间时做一次动作 */
    if (窗口已处理 != 0U) {
        return;
    }

    if (HAL_GetTick() < 灰度_上电窗口毫秒) {
        return;
    }

    窗口已处理 = 1U;

    /* 已经跑满窗口时间 -> 正常使用，不是"快速插拔"，把次数清零。
       正在校准或正在擦写 Flash 时不要插进来抢 Flash。 */
    if ((校准进行中 == 0U) && (正在写Flash == 0U)) {
        灰度_清上电次数();
    }
}

uint8_t 灰度_已校准(void)
{
    return 已校准;
}

void 灰度_恢复默认(void)
{
    uint16_t 记录[灰度_记录半字数];

    灰度_读记录(记录);

    记录[记录位_魔数]       = 灰度_魔数;
    记录[记录位_上电次数]   = 0U;
    记录[记录位_标志]       = 0U;
    记录[记录位_左电机速度] = (uint16_t)灰度_默认左电机速度;
    记录[记录位_右电机速度] = (uint16_t)灰度_默认右电机速度;
    记录[记录位_最低PWM]    = 灰度_默认最低PWM;
    /* 车库时间也是标定出来的东西，"恢复默认"就一并清掉：
       清掉之后 detective() 会自动退回经验阈值，不会不认路 */
    记录[记录位_车库40毫秒] = 0U;
    记录[记录位_车库25毫秒] = 0U;
    记录[记录位_校验]       = 灰度_算校验(记录);
    (void)灰度_写记录(记录);

    灰度_用默认阈值();
    已校准 = 0U;

    灰度_左电机速度 = 灰度_默认左电机速度;
    灰度_右电机速度 = 灰度_默认右电机速度;
    灰度_最低PWM    = 灰度_默认最低PWM;
    电机_设置最小占空比(灰度_最低PWM);
}

void 灰度_保存电机设置(void)
{
    uint16_t 记录[灰度_记录半字数];

    灰度_读记录(记录);

    /* 只更新电机相关字段，保留校准数据不动 */
    记录[记录位_左电机速度] = (uint16_t)灰度_左电机速度;
    记录[记录位_右电机速度] = (uint16_t)灰度_右电机速度;
    记录[记录位_最低PWM]    = 灰度_最低PWM;
    记录[记录位_校验]       = 灰度_算校验(记录);

    (void)灰度_写记录(记录);
}