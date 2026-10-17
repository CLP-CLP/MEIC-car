/**
 ******************************************************************************
 * @file    garage_calib.c
 * @brief   车库时间标定 + 共用尺子 侦探_走到横线()
 *
 *  两种车库（40 cm / 25 cm）各跑一次，把"最后那一段巡线"的耗时记下来
 *  存进 Flash，之后 detective() / 巡线检测停车() 就按这两个实测值分辨车库。
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "garage_calib.h"

#include "button.h"
#include "gray_calib.h"
#include "ir_sensor.h"
#include "line_follow.h"
#include "motor.h"
#include "oled.h"
#include "oled_text.h"
#include "turn.h"

#include <stdio.h>

#define 侦探_蹭线阈值       600     /* 中间三路大于这个值就算压上横线了 */

/* ====================== 共用尺子：侦探_走到横线 ============================ */
/*
 *  沿边巡线一个个路口走过去，每到一个路口就往前蹭一点点，
 *  看中间三路有没有压上横线（车库入口那条线）；
 *  压上了就说明到车库门口了，然后**再沿黑线走一小段**，
 *  记下这一小段花了多少毫秒 —— 两种车库长度不同（40 cm / 25 cm），
 *  这一段的耗时也就不同，用它来分辨是哪一种车库。
 *
 *  标定流程（车库时间标定）和正式跑车（detective）共用同一把尺子，
 *  改路线只改这一处。
 */
int 侦探_走到横线(void)
{
    while (1)
    {
        巡线(巡线_默认速度);

        /* 往前蹭一下，让探头能压到横线上 */
        电机_设置双轮(10, 10);
        HAL_Delay(60);
        电机_全部停止();
        HAL_Delay(80);

        if ((巡线_读取第几路(1) > 侦探_蹭线阈值)
         || (巡线_读取第几路(2) > 侦探_蹭线阈值)
         || (巡线_读取第几路(3) > 侦探_蹭线阈值)) {
            break;                      /* 到车库门口了 */
        }

        左转();                          /* 不是，继续走到下一个路口 */

        /* 长按确认键 = 中途放弃。标定的时候万一车跑飞了，不用去拔电。 */
        if (按键_取事件() == BTN_BACK) {
            电机_全部停止();
            return -1;
        }
    }

    return 巡线(10);                     /* 车库门口再走一小段，这一段就是"尺子" */
}

/* ====================== 调试用：车库时间自动标定 =========================== */

void 车库时间标定(void)
{
    char     行[24];
    uint16_t 时间40 = 0U;
    uint16_t 时间25 = 0U;
    uint8_t  写好了;

    printf("\r\n===== garage time calibration =====\r\n");

    /* ---- 第 1 趟：40 cm 车库 ---- */
    灰度_提示屏(文本_车库标定1, 文本_放到40车库, 文本_确认等于开跑);

    if (灰度_等确认() == 0U) {
        printf("garage calibration cancelled (step 1)\r\n");
        OLED_清屏();
        OLED_显示中文串(0, 0, 文本_已取消);
        OLED_刷新();
        HAL_Delay(800U);
        return;
    }

    HAL_Delay(500U);                    /* 手离开车，让它自己走 */
    {
        int 结果 = 侦探_走到横线();

        if (结果 < 0) {                 /* 中途长按确认放弃：不动 Flash */
            printf("garage calibration aborted during 40cm run\r\n");
            OLED_清屏();
            OLED_显示中文串(0, 0, 文本_已取消);
            OLED_刷新();
            HAL_Delay(800U);
            return;
        }
        时间40 = (uint16_t)结果;
    }
    printf("40cm run: %u ms\r\n", (unsigned)时间40);

    OLED_清屏();
    OLED_显示中文串(0, 0, 文本_车库标定1);
    (void)snprintf(行, sizeof(行), "40cm %u ms", (unsigned)时间40);
    OLED_显示ASCII串(0, 24, 行);
    OLED_刷新();
    HAL_Delay(1200U);                   /* 停一下让你看结果 */

    /* ---- 第 2 趟：25 cm 车库 ---- */
    灰度_提示屏(文本_车库标定2, 文本_放到25车库, 文本_确认等于开跑);

    if (灰度_等确认() == 0U) {
        printf("garage calibration cancelled (step 2)\r\n");
        OLED_清屏();
        OLED_显示中文串(0, 0, 文本_已取消);
        OLED_刷新();
        HAL_Delay(800U);
        return;
    }

    HAL_Delay(500U);
    {
        int 结果 = 侦探_走到横线();

        if (结果 < 0) {                 /* 中途长按确认放弃：不动 Flash */
            printf("garage calibration aborted during 25cm run\r\n");
            OLED_清屏();
            OLED_显示中文串(0, 0, 文本_已取消);
            OLED_刷新();
            HAL_Delay(800U);
            return;
        }
        时间25 = (uint16_t)结果;
    }
    printf("25cm run: %u ms\r\n", (unsigned)时间25);

    /* ---- 写 Flash ---- */
    写好了 = 灰度_存车库时间(时间40, 时间25);

    /* ---- 结果屏 ---- */
    OLED_清屏();
    OLED_显示中文串(0, 0, (写好了 != 0U) ? 文本_标定完成 : 文本_校准失败);

    (void)snprintf(行, sizeof(行), "40cm %u ms", (unsigned)时间40);
    OLED_显示ASCII串(0, 16, 行);

    (void)snprintf(行, sizeof(行), "25cm %u ms", (unsigned)时间25);
    OLED_显示ASCII串(0, 26, 行);

    (void)snprintf(行, sizeof(行), "mid  %u ms",
                   (unsigned)(((uint32_t)时间40 + (uint32_t)时间25) / 2U));
    OLED_显示ASCII串(0, 36, 行);

    OLED_显示中文串(0, 48, (写好了 != 0U) ? 文本_已存闪存 : 文本_没存上);
    OLED_刷新();

    HAL_Delay(2500U);                   /* 停住让你看清两个数 */

    printf("============================================\r\n\r\n");
}