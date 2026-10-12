/**
 ******************************************************************************
 * @file    usbd_conf.h
 * @brief   USB Device 协议栈配置头
 *
 *  ⚠ 文件名和路径都是中间件规定的：usbd_core.h 内部会
 *      #include "usbd_conf.h"
 *     所以这个文件必须放在 include/ 下（PlatformIO 已把 include/ 加进
 *     中间件的头文件搜索路径），不能改名、不能挪走。
 ******************************************************************************
 */

#ifndef __USBD_CONF_H
#define __USBD_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f1xx_hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================== 协议栈配置 ================================= */
#define USBD_MAX_NUM_INTERFACES         1U
#define USBD_MAX_NUM_CONFIGURATION      1U
#define USBD_MAX_STR_DESC_SIZ           0x100U
#define USBD_SUPPORT_USER_STRING_DESC   0U
#define USBD_DEBUG_LEVEL                0U
#define USBD_CDC_INTERVAL               2000U

/** 供电方式：0 = 总线供电（Blue Pill 由 USB 口取电），会影响 GET_STATUS 应答 */
#define USBD_SELF_POWERED               0U

/* ============================== 内存管理 =================================== */
/* CDC 类初始化时会 malloc 一个约 540 字节的句柄，
   本工程 retarget.c 已实现 _sbrk()，newlib 的 malloc 可直接使用 */
#define USBD_malloc                     malloc
#define USBD_free                       free
#define USBD_memset                     memset
#define USBD_memcpy                     memcpy

/* ================================ 日志 ===================================== */
#if (USBD_DEBUG_LEVEL > 0U)
#define USBD_UsrLog(...)   do { printf(__VA_ARGS__); printf("\r\n"); } while (0)
#define USBD_ErrLog(...)   do { printf("USB ERROR: "); printf(__VA_ARGS__); printf("\r\n"); } while (0)
#define USBD_DbgLog(...)   do { printf("USB DEBUG: "); printf(__VA_ARGS__); printf("\r\n"); } while (0)
#else
#define USBD_UsrLog(...)   do {} while (0)
#define USBD_ErrLog(...)   do {} while (0)
#define USBD_DbgLog(...)   do {} while (0)
#endif

/* ============================ USB 设备标识 ================================= */
/* VID/PID 沿用 ST 的 CDC 通用值，Windows 10/11 会自动装 usbser.sys，免驱动 */
#define USBD_VID                        0x0483U
#define USBD_PID                        0x5740U

#define USBD_LANGID_STRING              0x0409U     /* 美国英语 */

/**
  * @brief 设备字符串
  * @note  ⚠ 这里只能用 ASCII！
  *        USBD_GetString() 的实现是把每个字节直接扩成 UTF-16LE 的低字节，
  *        中文（UTF-8 多字节）会变成乱码显示在设备管理器 / 串口助手里。
  */
#define USBD_MANUFACTURER_STRING        "emic"
#define USBD_PRODUCT_STRING_FS          "STM32F103 Virtual COM Port"
#define USBD_CONFIGURATION_STRING_FS    "CDC Config"
#define USBD_INTERFACE_STRING_FS        "CDC Interface"

/* 字符串描述符索引 */
#define USBD_IDX_MFC_STR                0x01U
#define USBD_IDX_PRODUCT_STR            0x02U
#define USBD_IDX_SERIAL_STR             0x03U
#define USBD_IDX_CONFIG_STR             0x04U
#define USBD_IDX_INTERFACE_STR          0x05U

/** 序列号字符串长度：2 字节描述符头 + 12 组 UTF-16 字符 */
#define USB_SIZ_STRING_SERIAL           0x1AU

/* ============================== 全局句柄 =================================== */
/** USB 外设句柄，定义在 usbd_conf.c，供中断服务程序使用 */
extern PCD_HandleTypeDef hpcd_USB_FS;

#ifdef __cplusplus
}
#endif

#endif /* __USBD_CONF_H */
