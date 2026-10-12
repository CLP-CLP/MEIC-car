/**
 ******************************************************************************
 * @file    ir_sensor.c
 * @brief   五路模拟红外巡线传感器驱动（PA0 ~ PA4 / ADC1_IN0 ~ IN4）
 *
 *  读取流程（单路）
 *      巡线_读取引脚(引脚)
 *        -> 引脚号映射为 ADC 通道
 *        -> HAL_ADC_ConfigChannel()  选择通道与采样时间
 *        -> 第 1 次转换丢弃（清除上一通道残留在采样电容上的电荷）
 *        -> 巡线_采样次数 次转换取平均后返回
 *
 *  ⚠ 本文件使用中文标识符（UTF-8），需要 GCC 10 及以上。
 *     凡是以 HAL_ / 下划线开头、由 HAL 或 newlib 规定名字的函数，
 *     一律保持英文，否则会失去重定向 / 弱符号覆盖的效果。
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "ir_sensor.h"

#include <stddef.h>

/* ============================== 私有变量 =================================== */

/** ADC1 句柄（本模块私有，外部无需关心） */
static ADC_HandleTypeDef 巡线_ADC;

/** 五路传感器的引脚，顺序即 巡线_PA0 .. 巡线_PA4（下标 0~4） */
static const uint32_t 巡线_引脚表[巡线_路数] = {
    巡线_PA0,
    巡线_PA1,
    巡线_PA2,
    巡线_PA3,
    巡线_PA4,
};

/** 五路传感器相对车身中心线的横向位置（mm，左负右正）。
    按 ir_sensor.h 里的实际安装几何算：中间 2/3/4 路间距 20mm，
    两侧 1、5 路各甩出去 40mm —— 即 -60 / -20 / 0 / +20 / +60。
    （改了 巡线_间距_中间 / 巡线_间距_外侧，这里会自动跟着变） */
static const int16_t 巡线_位置表[巡线_路数] = {
    -(巡线_间距_中间 + 巡线_间距_外侧),     /* 下标 0 = 最左（第 1 路）*/
    -巡线_间距_中间,                        /* 下标 1 = 第 2 路 */
    0,                                      /* 下标 2 = 正中（第 3 路）*/
    +巡线_间距_中间,                        /* 下标 3 = 第 4 路 */
    +(巡线_间距_中间 + 巡线_间距_外侧),     /* 下标 4 = 最右（第 5 路）*/
};

/* ============================== 私有函数 =================================== */

/**
  * @brief  把 GPIO 引脚号映射为 ADC1 的通道号
  * @param  引脚  GPIO_PIN_x
  * @retval ADC_CHANNEL_x；该引脚没有对应的 ADC 通道时返回 巡线_通道无效
  * @note   仅适用于端口 A（PA0~PA7 -> ADC1_IN0~IN7）。
  *         注意 ADC_CHANNEL_0 的值就是 0，因此无效值必须用 0xFFFFFFFF 表示。
  */
static uint32_t 巡线_引脚转通道(uint32_t 引脚)
{
    switch (引脚)
    {
        case GPIO_PIN_0: return ADC_CHANNEL_0;
        case GPIO_PIN_1: return ADC_CHANNEL_1;
        case GPIO_PIN_2: return ADC_CHANNEL_2;
        case GPIO_PIN_3: return ADC_CHANNEL_3;
        case GPIO_PIN_4: return ADC_CHANNEL_4;
        case GPIO_PIN_5: return ADC_CHANNEL_5;
        case GPIO_PIN_6: return ADC_CHANNEL_6;
        case GPIO_PIN_7: return ADC_CHANNEL_7;
        default:         return 巡线_通道无效;
    }
}

/**
  * @brief  执行一次单次转换（软件触发 + 轮询）
  * @param  数值  输出：12 位原始值
  * @retval HAL_OK / HAL_ERROR / HAL_TIMEOUT
  */
static HAL_StatusTypeDef 巡线_单次转换(uint16_t *数值)
{
    if (HAL_ADC_Start(&巡线_ADC) != HAL_OK) {
        return HAL_ERROR;
    }

    if (HAL_ADC_PollForConversion(&巡线_ADC, 巡线_转换超时毫秒) != HAL_OK)
    {
        HAL_ADC_Stop(&巡线_ADC);
        return HAL_TIMEOUT;
    }

    *数值 = (uint16_t)HAL_ADC_GetValue(&巡线_ADC);
    HAL_ADC_Stop(&巡线_ADC);

    return HAL_OK;
}

/* ============================== 函数实现 =================================== */

void 巡线_初始化(void)
{
    /* 1. ADC1 基础配置：单通道、单次转换、软件触发、右对齐 */
    巡线_ADC.Instance                   = ADC1;
    巡线_ADC.Init.ScanConvMode          = ADC_SCAN_DISABLE;     /* 逐个通道轮询 */
    巡线_ADC.Init.ContinuousConvMode    = DISABLE;              /* 每次 Start 转换一次 */
    巡线_ADC.Init.DiscontinuousConvMode = DISABLE;
    巡线_ADC.Init.ExternalTrigConv      = ADC_SOFTWARE_START;   /* 软件触发 */
    巡线_ADC.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    巡线_ADC.Init.NbrOfConversion       = 1U;

    /* HAL_ADC_Init() 内部会调用 HAL_ADC_MspInit()：
       使能 ADC1 / GPIOA 时钟并把 PA0~PA4 配置为模拟输入 */
    if (HAL_ADC_Init(&巡线_ADC) != HAL_OK) {
        Error_Handler();
    }

    /* 2. 自校准：上电后执行一次，可显著改善精度（内部会自行开关 ADON） */
    if (HAL_ADCEx_Calibration_Start(&巡线_ADC) != HAL_OK) {
        Error_Handler();
    }
}

HAL_StatusTypeDef 巡线_读取引脚带状态(uint32_t 引脚, uint16_t *数值)
{
    ADC_ChannelConfTypeDef 通道配置 = {0};
    HAL_StatusTypeDef      状态;
    uint32_t               通道;
    uint32_t               累加 = 0U;
    uint16_t               单次值;
    uint8_t                i;

    if (数值 == NULL) {
        return HAL_ERROR;
    }

    通道 = 巡线_引脚转通道(引脚);
    if (通道 == 巡线_通道无效) {
        return HAL_ERROR;
    }

    /* 切换通道（同时设置采样时间） */
    通道配置.Channel      = 通道;
    通道配置.Rank         = ADC_REGULAR_RANK_1;
    通道配置.SamplingTime = 巡线_采样时间;
    if (HAL_ADC_ConfigChannel(&巡线_ADC, &通道配置) != HAL_OK) {
        return HAL_ERROR;
    }

    /* 关键：STM32F1 的 ADC 只有一个采样保持电容（ADC1/ADC2 共用）。
       切换通道后第一次转换会残留上一通道的电荷，必须丢弃，否则通道间串扰。
       若删掉这一行，相邻通道的读数会互相"污染"，表现为五路数值趋同。 */
    状态 = 巡线_单次转换(&单次值);
    if (状态 != HAL_OK) {
        return 状态;
    }

    /* 多次采样取平均，抑制红外模块与电源引入的噪声 */
    for (i = 0U; i < 巡线_采样次数; i++)
    {
        状态 = 巡线_单次转换(&单次值);
        if (状态 != HAL_OK) {
            return 状态;
        }
        累加 += 单次值;
    }

    *数值 = (uint16_t)(累加 / 巡线_采样次数);
    return HAL_OK;
}

uint16_t 巡线_读取引脚(uint32_t 引脚)
{
    uint16_t 数值 = 0U;

    /* 失败时保持 0：巡线应用里 0 即“无反射 / 最暗”，属于安全默认值 */
    (void)巡线_读取引脚带状态(引脚, &数值);
    return 数值;
}

uint16_t 巡线_读取第几路(uint8_t 第几路)
{
    if (第几路 >= 巡线_路数) {
        return 0U;              /* 序号越界，安全返回 0 */
    }

    return 巡线_读取引脚(巡线_引脚表[第几路]);
}

uint16_t 巡线_读取毫伏(uint32_t 引脚)
{
    uint16_t 原始值 = 巡线_读取引脚(引脚);

    return (uint16_t)(((uint32_t)原始值 * 巡线_参考电压毫伏) / 巡线_满量程);
}

void 巡线_读取全部(巡线_读数 *读数)
{
    uint8_t i;

    if (读数 == NULL) {
        return;
    }

    for (i = 0U; i < 巡线_路数; i++)
    {
        读数->原始值[i] = 巡线_读取引脚(巡线_引脚表[i]);
        读数->毫伏[i]   = (uint16_t)(((uint32_t)读数->原始值[i] * 巡线_参考电压毫伏)
                                     / 巡线_满量程);
    }
}

uint8_t 巡线_是否压线(uint16_t 原始值, uint16_t 阈值)
{
#if 巡线_黑线值更大
    return (原始值 >= 阈值) ? 1U : 0U;
#else
    return (原始值 <= 阈值) ? 1U : 0U;
#endif
}

int16_t 巡线_计算位置(const 巡线_读数 *读数, uint16_t 阈值)
{
    int32_t 位置和 = 0;
    int32_t 压线数 = 0;
    uint8_t i;

    if (读数 == NULL) {
        return 巡线_丢线;
    }

    for (i = 0U; i < 巡线_路数; i++)
    {
        if (巡线_是否压线(读数->原始值[i], 阈值))
        {
            位置和 += 巡线_位置表[i];
            压线数++;
        }
    }

    if (压线数 == 0) {
        return 巡线_丢线;           /* 五路都没压线 */
    }

    return (int16_t)(位置和 / 压线数);      /* 单位 mm，左负右正 */
}
