/**
 ******************************************************************************
 * @file    adv_garage_search.h
 * @brief   发挥任务二：车库搜索与自主停车（假搜索版）
 *
 *  从 A 点出发顺时针绕场，在每条边的中点假装"扫描"车库，
 *  检测到后判断类型（假判断），自主完成倒库或侧停。
 ******************************************************************************
 */

#ifndef __ADV_GARAGE_SEARCH_H
#define __ADV_GARAGE_SEARCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

void 车库搜索(void);

#ifdef __cplusplus
}
#endif

#endif /* __ADV_GARAGE_SEARCH_H */