/**
 ******************************************************************************
 * @file    my_follow.h
 * @brief   巡线 + 检测停车 —— 声明已迁移到 src/tasks/detect_park.h
 *
 *  保留本头文件作为兼容入口，实际声明在 src/tasks/ 目录中。
 ******************************************************************************
 */

#ifndef __MY_FOLLOW_H
#define __MY_FOLLOW_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* 实际声明在拆分后的任务模块中 */
#include "../src/tasks/detect_park.h"

#ifdef __cplusplus
}
#endif

#endif /* __MY_FOLLOW_H */