/**
 ******************************************************************************
 * @file    stm32f1xx_it.h
 * @brief   中断服务程序声明
 ******************************************************************************
 */

#ifndef __STM32F1xx_IT_H
#define __STM32F1xx_IT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Cortex-M3 内核异常 --------------------------------------------------------*/
void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void SVC_Handler(void);
void DebugMon_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);

/* 外设中断 ------------------------------------------------------------------*/
void USB_LP_CAN1_RX0_IRQHandler(void);
void USB_HP_CAN1_TX_IRQHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* __STM32F1xx_IT_H */
