/**
 ******************************************************************************
 * @file    board.h
 * @brief   板级初始化（系统时钟、板载 LED）
 *
 *  main.c 里只留一行 板级_初始化()，具体的时钟树与 GPIO 配置都在 board.c。
 ******************************************************************************
 */

#ifndef __BOARD_H
#define __BOARD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/**
  * @brief  板级初始化：系统时钟 72 MHz + 板载 LED (PC13)
  * @note   必须在 HAL_Init() 之后调用。
  *         内部依次执行 SystemClock_Config() 与 MX_GPIO_Init()（见 board.c）。
  *         PA0~PA4 归巡线传感器、PA11/PA12 归 USB，均不在这里配置。
  */
void 板级_初始化(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_H */
