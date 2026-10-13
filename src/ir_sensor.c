/**
 ******************************************************************************
 * @file    ir_sensor.c
 * @brief   五路模拟红外巡线传感器驱动（PA0 ~ PA4 / ADC1_IN0 ~ IN4）
 *
 *  读取流程
 *      ── 巡线_用DMA = 1（默认）──
 *          巡线_初始化()：ADC1 = 扫描 + 连续转换 + DMA1_Channel1 循环搬运，
 *                        排成 ch0,ch0,ch0, ch1,ch1,ch1, ... 共 15 个转换
 *          硬件循环跑：每 105 µs 搬完一轮 -> DMA1_Channel1 中断
 *          DMA1_Channel1_IRQHandler -> 巡线_DMA完成中断()
 *                        -> 把每一路"第一次丢弃后的两个样本"取平均
 *                        -> 写进 巡线_最新帧[0..4]
 *          主循环：巡线_读取第几路() / 巡线_取最新帧() 只读内存，不阻塞
 *
 *      ── 巡线_用DMA = 0（兼容旧行为）──
 *          巡线_读取引脚带状态() 里逐通道轮询，每次切换通道后先丢弃一次转换，
 *          再采样 巡线_采样次数 次取平均，最后返回。
 *
 *  ⚠ 关于"用中断"
 *      五路传感器是**模拟输出**，PA0~PA4 工作在模拟输入模式下（施密特触发器
 *      关闭），模拟脚上挂不了 EXTI —— 所以"传感器一变黑就进中断"在硬件上做不到。
 *      能做的等价物就是这里用的：让 ADC + DMA 在后台一直采，数据一好就由
 *      DMA 传输完成中断把它整理成"最新帧"，主循环再也不需要等 ADC。
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

#if 巡线_用DMA
/** ADC1 的 DMA 通道（F103 上 ADC1 固定用 DMA1_Channel1） */
static DMA_HandleTypeDef 巡线_DMA句柄;

/** DMA 落点：一次完整扫描的全部原始值（顺序见 巡线_每路序号 的注释） */
static volatile uint16_t 巡线_扫描缓冲[巡线_扫描长度];

/** 整理后的"最新一帧"：下标 0~4 = 第 1~5 路（中断里更新，主循环里读） */
static volatile uint16_t 巡线_最新帧[巡线_路数];
#endif

#if !巡线_用DMA
/** 五路传感器的引脚，顺序即 巡线_PA0 .. 巡线_PA4（下标 0~4）
    （只有轮询方式才需要它：DMA 方式的通道号就是下标本身） */
static const uint32_t 巡线_引脚表[巡线_路数] = {
    巡线_PA0,
    巡线_PA1,
    巡线_PA2,
    巡线_PA3,
    巡线_PA4,
};
#endif

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
  * @note   只有 巡线_用DMA = 0 的轮询方式才用得到
  */
#if !巡线_用DMA
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
#endif  /* !巡线_用DMA */

/* ============================== 函数实现 =================================== */

void 巡线_初始化(void)
{
    /* 1. ADC1 基础配置 */
    巡线_ADC.Instance = ADC1;

#if 巡线_用DMA
    /* ---- DMA 方式：扫描 5 路 + 连续转换 + 循环 DMA，CPU 不参与 ---- */
    巡线_ADC.Init.ScanConvMode          = ADC_SCAN_ENABLE;      /* 5 路轮流扫 */
    巡线_ADC.Init.ContinuousConvMode    = ENABLE;               /* 一轮完了自动再来 */
    巡线_ADC.Init.DiscontinuousConvMode = DISABLE;
    巡线_ADC.Init.ExternalTrigConv      = ADC_SOFTWARE_START;   /* 软件启动一次就够 */
    巡线_ADC.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    巡线_ADC.Init.NbrOfConversion       = 巡线_扫描长度;        /* 5 路 × 每路 3 次 = 15 */

    /* DMA 参数先备好并链接到 ADC 句柄：
       必须在 HAL_ADC_Init() 之前 __HAL_LINKDMA()，因为下面 HAL_ADC_Init()
       会调用 HAL_ADC_MspInit()，那里顺手把 DMA1 的时钟打开；
       然后 HAL_DMA_Init() 才能配置寄存器。 */
    巡线_DMA句柄.Instance                 = DMA1_Channel1;
    巡线_DMA句柄.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    巡线_DMA句柄.Init.PeriphInc           = DMA_PINC_DISABLE;   /* 源固定 = ADC->DR */
    巡线_DMA句柄.Init.MemInc              = DMA_MINC_ENABLE;    /* 落点逐个往后写 */
    巡线_DMA句柄.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    巡线_DMA句柄.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    巡线_DMA句柄.Init.Mode                = DMA_CIRCULAR;       /* 循环，永不停止 */
    巡线_DMA句柄.Init.Priority            = DMA_PRIORITY_HIGH;

    __HAL_LINKDMA(&巡线_ADC, DMA_Handle, 巡线_DMA句柄);
#else
    /* ---- 轮询方式：单通道、单次转换、软件触发 ---- */
    巡线_ADC.Init.ScanConvMode          = ADC_SCAN_DISABLE;     /* 逐个通道轮询 */
    巡线_ADC.Init.ContinuousConvMode    = DISABLE;              /* 每次 Start 转换一次 */
    巡线_ADC.Init.DiscontinuousConvMode = DISABLE;
    巡线_ADC.Init.ExternalTrigConv      = ADC_SOFTWARE_START;   /* 软件触发 */
    巡线_ADC.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    巡线_ADC.Init.NbrOfConversion       = 1U;
#endif

    /* HAL_ADC_Init() 内部会调用 HAL_ADC_MspInit()：
       使能 ADC1 / DMA1 / GPIOA 时钟并把 PA0~PA4 配置为模拟输入 */
    if (HAL_ADC_Init(&巡线_ADC) != HAL_OK) {
        Error_Handler();
    }

#if 巡线_用DMA
    /* 2. 配置 DMA 通道（时钟已在 HAL_ADC_MspInit 里打开） */
    if (HAL_DMA_Init(&巡线_DMA句柄) != HAL_OK) {
        Error_Handler();
    }

    /* 3. 自校准：上电后执行一次，可显著改善精度（内部会自行开关 ADON） */
    if (HAL_ADCEx_Calibration_Start(&巡线_ADC) != HAL_OK) {
        Error_Handler();
    }

    /* 4. 排扫描序列：每一路连着占 巡线_每路序号 个 Rank。
          切换通道后的第一个转换会被上一路的残压污染，所以只用后面几个。 */
    {
        ADC_ChannelConfTypeDef 通道配置 = {0};
        uint8_t 第几路;
        uint8_t 序号;

        通道配置.SamplingTime = 巡线_采样时间;

        for (第几路 = 0U; 第几路 < 巡线_路数; 第几路++)
        {
            for (序号 = 0U; 序号 < 巡线_每路序号; 序号++)
            {
                /* 引脚表顺序就是 PA0..PA4，正好是 ADC1_IN0..IN4 */
                通道配置.Channel = (uint32_t)第几路;
                通道配置.Rank    = (uint32_t)(第几路 * 巡线_每路序号 + 序号) + 1U;

                if (HAL_ADC_ConfigChannel(&巡线_ADC, &通道配置) != HAL_OK) {
                    Error_Handler();
                }
            }
        }
    }

    /* 5. 开机：启动循环 DMA，之后 ADC 自己一轮接一轮地扫，
         每扫完一轮进一次 DMA1_Channel1 中断更新 巡线_最新帧[]。
         这里返回后 CPU 就再也不需要碰 ADC 了。 */
    HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 巡线_DMA中断优先级, 0U);
    HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);

    if (HAL_ADC_Start_DMA(&巡线_ADC, (uint32_t *)巡线_扫描缓冲, 巡线_扫描长度) != HAL_OK) {
        Error_Handler();
    }

    /* HAL_ADC_Start_DMA() 会顺手把 DMA 的"半传输中断"也打开（它设了半完成回调）。
       我们只要"整圈扫完"这一个点：半圈时第 1~3 路是新的、第 4~5 路还是上一圈的，
       按半圈更新会把两圈的样本混进同一帧。所以这里关掉半传输中断。 */
    __HAL_DMA_DISABLE_IT(&巡线_DMA句柄, DMA_IT_HT);
    __HAL_DMA_CLEAR_FLAG(&巡线_DMA句柄, DMA_FLAG_HT1);
#else
    /* 2. 自校准：上电后执行一次，可显著改善精度（内部会自行开关 ADON） */
    if (HAL_ADCEx_Calibration_Start(&巡线_ADC) != HAL_OK) {
        Error_Handler();
    }
#endif
}

#if 巡线_用DMA
/**
  * @brief  一轮扫描搬完：把每路"冲洗后的样本"取平均，更新最新帧
  * @note   由 DMA1_Channel1_IRQHandler 调用（见 stm32f1xx_it.c）。
  *         中断里只做定长整数运算，耗时 ~1 µs，而一轮是 105 µs，占用很低。
  *
  *         这里不做"新旧帧跨帧平均"：跨帧平均等于给检测人为加延迟，
  *         而路口检测最怕的就是延迟 —— 噪声交给上层的"连续确认窗口"去滤。
  */
void 巡线_DMA完成中断(void)
{
    uint8_t 第几路;

    /* 让 HAL 清 DMA 标志、走一遍回调（循环模式下不会关掉通道） */
    HAL_DMA_IRQHandler(&巡线_DMA句柄);

    for (第几路 = 0U; 第几路 < 巡线_路数; 第几路++)
    {
        uint32_t 累加 = 0U;
        uint8_t  序号;

        /* 序号 0 是通道切换后的第一次转换，丢弃；其余取平均 */
        for (序号 = 1U; 序号 < 巡线_每路序号; 序号++)
        {
            累加 += (uint32_t)巡线_扫描缓冲[第几路 * 巡线_每路序号 + 序号];
        }

        巡线_最新帧[第几路] =
            (uint16_t)(累加 / (uint32_t)(巡线_每路序号 - 1U));
    }
}
#else
void 巡线_DMA完成中断(void)
{
    /* 巡线_用DMA = 0 时不用 DMA，这个中断也不会被使能，留个空壳只为链接得上 */
}
#endif

HAL_StatusTypeDef 巡线_读取引脚带状态(uint32_t 引脚, uint16_t *数值)
{
    uint32_t 通道;

    if (数值 == NULL) {
        return HAL_ERROR;
    }

    通道 = 巡线_引脚转通道(引脚);
    if (通道 == 巡线_通道无效) {
        return HAL_ERROR;
    }

#if 巡线_用DMA
    /* DMA 方式下 ADC1 正被循环扫描占着，没法再"单独轮询某个通道"。
       好在这五路本来就在扫描序列里，直接返回最新帧里的对应值即可。 */
    if (通道 <= ADC_CHANNEL_4) {
        *数值 = 巡线_最新帧[通道];
        return HAL_OK;
    }
    return HAL_ERROR;       /* PA5~PA7 不在扫描序列里 */
#else
    {
        ADC_ChannelConfTypeDef 通道配置 = {0};
        HAL_StatusTypeDef      状态;
        uint32_t               累加 = 0U;
        uint16_t               单次值;
        uint8_t                i;

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
#endif
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

#if 巡线_用DMA
    /* 直接读最新帧：几条指令，不阻塞、不碰 ADC */
    return 巡线_最新帧[第几路];
#else
    return 巡线_读取引脚(巡线_引脚表[第几路]);
#endif
}

void 巡线_取最新帧(uint16_t *原始值)
{
    uint8_t i;

    if (原始值 == NULL) {
        return;
    }

    for (i = 0U; i < 巡线_路数; i++)
    {
#if 巡线_用DMA
        原始值[i] = 巡线_最新帧[i];
#else
        原始值[i] = 巡线_读取引脚(巡线_引脚表[i]);
#endif
    }
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

    /* 先用 巡线_取最新帧() 把五路一次性取回，保证五路是同一帧的 */
    巡线_取最新帧(读数->原始值);

    for (i = 0U; i < 巡线_路数; i++)
    {
        读数->毫伏[i] = (uint16_t)(((uint32_t)读数->原始值[i] * 巡线_参考电压毫伏)
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
