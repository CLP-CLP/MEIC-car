/**
 ******************************************************************************
 * @file    adv_laser_draw.h
 * @brief   发挥任务三：二维云台激光绘图
 *
 *  小车装载二维云台 + 蓝紫激光笔，绕场一周期间在 AB 中点外侧
 *  50cm 处靶纸上画出图形（三角形/正方形/圆形/正弦波）。
 ******************************************************************************
 */

#ifndef __ADV_LASER_DRAW_H
#define __ADV_LASER_DRAW_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/**
  * @brief  激光绘图任务入口
  * @param  发车点  'A' / 'B' / 'C' / 'D'
  *          A → 正三角形，B → 正方形，C → 圆形，D → 正弦波
  */
void 激光绘图(char 发车点);

#ifdef __cplusplus
}
#endif

#endif /* __ADV_LASER_DRAW_H */