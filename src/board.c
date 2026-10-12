/**
 ******************************************************************************
 * @file    board.c
 * @brief   板级初始化：系统时钟、板载 LED，以及全局错误处理
 *
 *  从 main.c 搬出来的内容，函数名沿用 STM32CubeMX 的习惯写法，
 *  方便和网上的教程 / CubeMX 生成的代码对照。
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "board.h"

#include <stdio.h>

/* ============================ 私有函数声明 ================================= */
static void SystemClock_Config(void);
static void MX_GPIO_Init(void);

/* ============================== 对外接口 =================================== */

void 板级_初始化(void)
{
    SystemClock_Config();       /* HSE 8 MHz + PLL x9 = 72 MHz（USB 时钟 48 MHz）*/
    MX_GPIO_Init();             /* 板载 LED：PC13 */
}

/* ============================== 系统时钟 =================================== */
/**
  * @brief  配置 SYSCLK = 72 MHz
  *         HSE(8 MHz) -> PLL x9 = 72 MHz，AHB=72 / APB1=36 / APB2=72 MHz
  *         Flash 等待周期 = 2（>48 MHz 必须）
  * @note   USB 需要的 48 MHz = 72 MHz / 1.5，在 stm32f1xx_hal_msp.c 的
  *         HAL_MspInit() 里配置
  */
static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    /* --- 第一步：配置振荡器与 PLL --- */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState       = RCC_HSE_ON;
    RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    RCC_OscInitStruct.HSIState       = RCC_HSI_ON;
    RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLMUL     = RCC_PLL_MUL9;      /* 8 MHz x 9 = 72 MHz */

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        /* HSE 起振失败（晶振未焊、负载电容异常等）时回退到 HSI：
           注意：没有外部晶振时 USB 将无法工作（USB 要求 48 MHz ±0.25%） */
        RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
        RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
        RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
        RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
        RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_HSI_DIV2;
        RCC_OscInitStruct.PLL.PLLMUL          = RCC_PLL_MUL16;  /* 4 MHz x 16 = 64 MHz */

        if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
            Error_Handler();
        }
    }

    /* --- 第二步：配置总线分频并切换系统时钟源 --- */
    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;   /* PCLK1 最高 36 MHz */
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;   /* PCLK2 最高 72 MHz */

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) {
        Error_Handler();
    }
}

/* ============================== GPIO 初始化 ================================ */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    /* 使能 GPIO 时钟（GPIOA 由 ADC / USB 各自的 MspInit 使能） */
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* PC13：板载 LED，推挽输出，低电平点亮；上电先熄灭 */
    LED_OFF();
    GPIO_InitStruct.Pin   = LED_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_GPIO_PORT, &GPIO_InitStruct);

    /* PA0 ~ PA4 为红外巡线传感器的模拟输入，由 巡线_初始化() 配置 */
}

/* ============================== 错误处理 =================================== */
/**
  * @brief  致命错误处理：关闭中断并快速闪烁 LED
  * @note   可能被中断上下文调用，因此不能用 HAL_Delay()
  *         函数名沿用 CubeMX 约定，被 ir_sensor.c / vcp.c / usbd_conf.c /
  *         stm32f1xx_hal_msp.c 共同调用，声明在 main.h
  */
void Error_Handler(void)
{
    __disable_irq();

    for (;;)
    {
        LED_TOGGLE();
        for (volatile uint32_t i = 0; i < 200000U; i++) {
            __NOP();
        }
    }
}

#ifdef USE_FULL_ASSERT
/**
  * @brief  assert_param() 失败时的回调（在 stm32f1xx_hal_conf.h 中打开宏后生效）
  */
void assert_failed(uint8_t *file, uint32_t line)
{
    printf("ASSERT FAILED: %s, line %lu\r\n", (const char *)file, (unsigned long)line);
    Error_Handler();
}
#endif /* USE_FULL_ASSERT */
