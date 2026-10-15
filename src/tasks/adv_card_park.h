/**
 ******************************************************************************
 * @file    adv_card_park.h
 * @brief   发挥任务一：卡片识别动态车库（假动作版）
 *
 *  不做真实视觉识别，用云台摆动假装"扫描卡片"，
 *  固定返回"红圆→BC倒库"，然后调用已有倒库逻辑。
 ******************************************************************************
 */

#ifndef __ADV_CARD_PARK_H
#define __ADV_CARD_PARK_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

void 卡片入库(void);

#ifdef __cplusplus
}
#endif

#endif /* __ADV_CARD_PARK_H */