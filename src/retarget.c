/**
 ******************************************************************************
 * @file    retarget.c
 * @brief   标准库重定向：printf 输出到 USB 虚拟串口，并为 malloc 提供堆
 *
 *  链路：printf() -> _write() -> 虚拟串口_发送() -> 环形缓冲 -> USB -> 电脑
 *
 *  注意：_write / _sbrk 是 newlib 规定的接口名，不能改成中文，
 *        否则 printf 会没有输出、malloc 会失败。
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "vcp.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>

/* 由链接脚本 STM32F103C8TX_FLASH.ld 提供的符号 ------------------------------*/
/* 注意：链接符号只是地址，必须声明为“不完整数组类型”，
   否则 GCC 会认为 _estack 是 char[1] 并在做减法时报 -Warray-bounds 告警 */
extern char _end[];     /* 堆起始地址（.bss 之后） */
extern char _estack[];  /* RAM 顶部（栈顶） */

#define MIN_STACK_RESERVE   (0x400U)    /* 为栈保留的最小空间 */

/* ============================== 重定向实现 ================================= */

/**
  * @brief  newlib 的 printf/puts 等最终都调用 _write()，这里重定向到虚拟串口
  * @param  file 文件描述符（未使用）
  * @param  ptr  待发送数据
  * @param  len  数据长度
  * @retval 始终返回 len
  * @note   虚拟串口_发送() 是非阻塞的：数据只进环形缓冲，缓冲满了会丢弃。
  *         这里仍然返回 len，是为了让 newlib 认为写成功、不要重试或报错 ——
  *         调试打印丢几行没关系，主循环被卡住才是问题。
  */
int _write(int file, char *ptr, int len)
{
    (void)file;

    if ((ptr != NULL) && (len > 0)) {
        (void)虚拟串口_发送((const uint8_t *)ptr, (uint16_t)len);
    }

    return len;
}

/**
  * @brief  newlib malloc() 的堆扩展接口
  * @param  incr 本次需要增加的字节数
  * @retval 分配到的内存首地址，失败返回 (void *)-1
  * @note   USB Device 库初始化 CDC 类时会 malloc 约 540 字节，必须能成功
  */
void *_sbrk(ptrdiff_t incr)
{
    static char *heap_end = NULL;
    char        *prev;

    if (heap_end == NULL) {
        heap_end = _end;
    }

    prev = heap_end;

    /* 堆向上生长，不能侵占为栈保留的空间 */
    if ((heap_end + incr) > (_estack - MIN_STACK_RESERVE)) {
        errno = ENOMEM;
        return (void *)-1;
    }

    heap_end += incr;
    return (void *)prev;
}
