/**
 ******************************************************************************
 * @file    stm32f1xx_hal_msp.c
 * @brief   MCU 支持包（MSP）：外设的底层初始化——时钟使能、引脚复用、NVIC
 *
 *  USB 的底层初始化在 usbd_conf.c 的 HAL_PCD_MspInit() 里（USB 库要求放那里）。
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "ir_sensor.h"

/**
  * @brief  全局 MSP 初始化，由 HAL_Init() 调用
  */
void HAL_MspInit(void)
{
    RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

    __HAL_RCC_AFIO_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();

    /* 关闭 JTAG，仅保留 SWD 调试口（释放 PA15 / PB3 / PB4 作普通 IO 使用）
       注意：PA11/PA12 是 USB D-/D+，不受 JTAG 影响；PA13/PA14 保持 SWD 可用 */
    __HAL_AFIO_REMAP_SWJ_NOJTAG();

    /* ---- 外设专用时钟配置 ----
       本函数在 HAL_Init() 内部被调用（早于 SystemClock_Config()），
       这里只写分频寄存器，等 PLL 起来后自动生效：
         USB：72 MHz / 1.5 = 48 MHz（USB 全速要求 48 MHz ±0.25%）
         ADC：72 MHz / 6   = 12 MHz（F1 规定 ADC 时钟不得超过 14 MHz）   */
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USB | RCC_PERIPHCLK_ADC;
    PeriphClkInit.UsbClockSelection    = RCC_USBCLKSOURCE_PLL_DIV1_5;
    PeriphClkInit.AdcClockSelection    = RCC_ADCPCLK2_DIV6;

    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK) {
        Error_Handler();
    }
}

/**
  * @brief  ADC1 底层初始化：五路红外巡线传感器接在 PA0 ~ PA4
  */
void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (hadc->Instance == ADC1)
    {
        __HAL_RCC_ADC1_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();

        /* ADC1 的扫描结果由 DMA1_Channel1 循环搬进内存（见 ir_sensor.c）：
           F103 上 ADC1 只能走 DMA1 通道 1，这里把它的时钟一起打开。
           巡线_用DMA = 0（轮询方式）时这句是多余的，开着也无害。 */
        __HAL_RCC_DMA1_CLK_ENABLE();

        /* PA0 ~ PA4 配置为模拟输入（浮空，无上下拉），
           模拟模式下施密特触发器关闭，不会给传感器带来额外负载 */
        GPIO_InitStruct.Pin  = 巡线_全部引脚;
        GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(巡线_端口, &GPIO_InitStruct);
    }
}

/**
  * @brief  ADC1 底层反初始化
  */
void HAL_ADC_MspDeInit(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)
    {
        __HAL_RCC_ADC1_CLK_DISABLE();

        HAL_GPIO_DeInit(巡线_端口, 巡线_全部引脚);
    }
}

/**
  * @brief  TIM PWM 底层初始化：L298N 的使能脚接 PA6 / PA7
  *         PA6 = TIM3_CH1 = ENA（左电机）
  *         PA7 = TIM3_CH2 = ENB（右电机）      详见 motor.h
  */
void HAL_TIM_PWM_MspInit(TIM_HandleTypeDef *htim)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (htim->Instance == TIM3)
    {
        __HAL_RCC_TIM3_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();

        /* STM32F1 没有引脚复用选择寄存器（AFR），
           直接把引脚配成"复用推挽输出"即可输出 PWM */
        GPIO_InitStruct.Pin   = GPIO_PIN_6 | GPIO_PIN_7;
        GPIO_InitStruct.Mode  = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull  = GPIO_NOPULL;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
    }
}
