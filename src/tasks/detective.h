/**
 ******************************************************************************
 * @file    detective.h
 * @brief   自定义任务：巡线到横线，按耗时分辨车库类型（倒库 / 侧方）
 ******************************************************************************
 */

#ifndef __DETECTIVE_H
#define __DETECTIVE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/**
  * @brief  巡线到车库门口，按"最后一段巡线耗时"分辨 40cm/25cm 车库，
  *         自动选择倒库或侧方停车。
  * @note   耗时来自 Flash 标定值（车库时间标定），没标定过则退回 1000ms 经验阈值。
  */
void detective(void);

#ifdef __cplusplus
}
#endif

#endif /* __DETECTIVE_H */