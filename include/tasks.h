/**
 ******************************************************************************
 * @file    tasks.h
 * @brief   meic 原有的三个任务函数声明（实现都在 src/main.c 里）
 *
 *  菜单（menu.c）通过本头文件调用它们。
 ******************************************************************************
 */

#ifndef __TASKS_H
#define __TASKS_H

#ifdef __cplusplus
extern "C" {
#endif

void 倒库(void);                /* 基础任务二 · 倒车入库 */
void 侧方停车(void);            /* 基础任务二 · 侧方停车 */
void 任务三_定点停车(void);     /* 基础任务三 · 罗马数字识别定点停车 */
void detective(void);           /* 自定义：巡线到横线，再按用时选倒库 / 侧方 */

/*
 *  注意：任务函数的**实现都写在 src/main.c 里**，本头文件只放声明。
 *  千万不要在 menu.c（或别的 .c）里再写一份同名函数体，否则两个 .o 里都有
 *  这个全局符号，链接会报 "multiple definition of ..."。
 */

#ifdef __cplusplus
}
#endif

#endif /* __TASKS_H */
