/**
 ******************************************************************************
 * @file    stm32f1xx_it.c
 * @brief   中断服务程序（ISR）
 *
 *  说明：启动文件 startup_stm32f103xb.S 中所有中断向量都是 weak 定义，
 *        这里用同名强定义覆盖。函数名由启动文件和 HAL 规定，不能改名。
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32f1xx_it.h"
#include "gray_calib.h"
#include "usbd_conf.h"
#include "vcp.h"

/* ======================= Cortex-M3 处理器异常 ============================== */

void NMI_Handler(void)
{
    while (1)
    {
    }
}

/**
  * @brief  硬件错误：调试时在此打断点，检查 SCB->CFSR / HFSR 定位原因
  */
void HardFault_Handler(void)
{
    while (1)
    {
    }
}

void MemManage_Handler(void)
{
    while (1)
    {
    }
}

void BusFault_Handler(void)
{
    while (1)
    {
    }
}

void UsageFault_Handler(void)
{
    while (1)
    {
    }
}

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}

/* ============================== 外设中断 =================================== */

/**
  * @brief  SysTick 中断（1 ms）：
  *           1) HAL_IncTick()    维持 HAL_GetTick() / HAL_Delay()
  *           2) 虚拟串口_轮询()  推进 USB 发送，这样 printf 才能不阻塞主循环
  */
void SysTick_Handler(void)
{
    HAL_IncTick();

    虚拟串口_轮询();

    灰度_窗口检查();        /* 上电满 2 秒就把"连续上电次数"清零（正常使用不误触发校准） */
}

/**
  * @brief  USB 低优先级中断（与 CAN1_RX0 共用向量）：
  *         USB 的全部枚举、收发状态机都在这里由 HAL 处理
  */
void USB_LP_CAN1_RX0_IRQHandler(void)
{
    HAL_PCD_IRQHandler(&hpcd_USB_FS);
}

/**
  * @brief  USB 高优先级中断（与 CAN1_TX 共用向量）。
  *         CDC（批量/中断端点）只用到低优先级中断，这里保留定义是为了
  *         万一将来启用双缓冲/同步端点时不至于落进默认死循环
  */
void USB_HP_CAN1_TX_IRQHandler(void)
{
    HAL_PCD_IRQHandler(&hpcd_USB_FS);
}
