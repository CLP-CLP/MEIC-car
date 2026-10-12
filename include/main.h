/**
 ******************************************************************************
 * @file    main.h
 * @brief   全局头文件：板级引脚定义与公共声明
 *
 *  各模块的头文件：
 *      ir_sensor.h   五路模拟红外巡线传感器
 *      vcp.h         USB 虚拟串口
 ******************************************************************************
 */

#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f1xx_hal.h"
#include <stdint.h>

/* ======================= 板级硬件定义 (Blue Pill F103C8T6) ================= */

/** 板载 LED：PC13，低电平点亮 */
#define LED_PIN                 GPIO_PIN_13
#define LED_GPIO_PORT           GPIOC
#define LED_ON()                HAL_GPIO_WritePin(LED_GPIO_PORT, LED_PIN, GPIO_PIN_RESET)
#define LED_OFF()               HAL_GPIO_WritePin(LED_GPIO_PORT, LED_PIN, GPIO_PIN_SET)
#define LED_TOGGLE()            HAL_GPIO_TogglePin(LED_GPIO_PORT, LED_PIN)
#define LED_IS_ON()             (HAL_GPIO_ReadPin(LED_GPIO_PORT, LED_PIN) == GPIO_PIN_RESET)

/* ============================== 引脚占用一览 ===============================
 *  PA0  ~ PA4   五路模拟红外巡线传感器（ADC1_IN0 ~ IN4）     见 ir_sensor.h
 *  PA6  / PA7   TIM3_CH1 / CH2 -> L298N ENA / ENB（PWM 调速） 见 motor.h
 *  PA9  / PA10  USART1 TX/RX（本工程未使用，留给以后接 USB-TTL）
 *  PA11 / PA12  USB D- / D+，即虚拟串口                       见 vcp.h
 *  PA13 / PA14  SWD 调试口（SWDIO / SWCLK），不可占用
 *  PB0  / PB1   L298N IN1 / IN2（左电机方向）                 见 motor.h
 *  PB6  / PB7   I2C1_SCL / SDA（OLED）                        见 oled.h
 *  PB10 / PB11  L298N IN3 / IN4（右电机方向）                 见 motor.h
 *  PB12         上键（菜单光标上移）                           见 button.h
 *  PB13         下键（菜单光标下移）                           见 button.h
 *  PB14         确认键（短按=进入/执行，长按=返回）            见 button.h
 *  PC13         板载 LED
 * ========================================================================== */

/* =============================== 导出函数 ================================== */
void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */