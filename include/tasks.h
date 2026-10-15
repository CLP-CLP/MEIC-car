/**
 ******************************************************************************
 * @file    tasks.h
 * @brief   任务函数声明总入口 —— 包含所有拆分的任务子模块头文件
 *
 *  菜单（menu_tree.c）通过本头文件调用各任务。
 *  各任务的实现已拆分到 src/tasks/ 目录下各自的 .c / .h 文件中。
 ******************************************************************************
 */

#ifndef __TASKS_H
#define __TASKS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 基础任务二：倒车入库 / 侧方停车 ---- */
#include "../src/tasks/park_back.h"

/* ---- 基础任务三：罗马数字识别定点停车 ---- */
#include "../src/tasks/task3_roman.h"

/* ---- 自定义任务：侦探（按耗时分辨车库类型） ---- */
#include "../src/tasks/detective.h"

/* ---- 调试：车库时间标定 ---- */
#include "../src/tasks/garage_calib.h"

/* ---- 发挥任务一：卡片识别动态车库（假动作） ---- */
#include "../src/tasks/adv_card_park.h"

/* ---- 发挥任务二：车库搜索与自主停车 ---- */
#include "../src/tasks/adv_garage_search.h"

/* ---- 发挥任务三：二维云台激光绘图 ---- */
#include "../src/tasks/adv_laser_draw.h"

/*
 *  原来这些声明直接写在 tasks.h 里，实现放在 main.c 里；
 *  现在实现拆到了 src/tasks/ 目录，每个模块有自己独立的头文件。
 */

#ifdef __cplusplus
}
#endif

#endif /* __TASKS_H */