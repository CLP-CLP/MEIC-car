/**
 ******************************************************************************
 * @file    vcp.c
 * @brief   USB 虚拟串口（USB CDC / Virtual COM Port）实现
 *
 *  数据流
 *      printf() -> _write() [retarget.c]
 *               -> 虚拟串口_发送()      只写环形缓冲，立即返回（不阻塞）
 *               -> 虚拟串口_轮询()      SysTick 每 1 ms 调一次，组包
 *               -> USBD_CDC_TransmitPacket() -> USB 总线 -> 电脑
 *
 *  为什么不做成"直接发送"：
 *      电脑没打开串口、或串口被占用时，主机会一直 NAK，USB 传输永远不完成。
 *      若 printf 里同步等待，主循环会被拖死（这对巡线小车是致命的）。
 *      因此这里用"环形缓冲 + 非阻塞 + 满了就丢"，printf 永远不会卡住。
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usbd_conf.h"
#include "usbd_core.h"
#include "usbd_cdc.h"
#include "usbd_ctlreq.h"        /* USBD_GetString */
#include "vcp.h"

/* ============================== 私有变量 =================================== */

/** USB 协议栈句柄 */
static USBD_HandleTypeDef  虚拟串口_设备;

/** CDC 接口回调表（4 个成员的名字由本文件自己决定） */
static int8_t 虚拟串口_接口初始化(void);
static int8_t 虚拟串口_接口反初始化(void);
static int8_t 虚拟串口_接口控制(uint8_t 命令, uint8_t *数据, uint16_t 长度);
static int8_t 虚拟串口_接口接收(uint8_t *数据, uint32_t *长度);

static USBD_CDC_ItfTypeDef 虚拟串口_CDC接口 =
{
    虚拟串口_接口初始化,
    虚拟串口_接口反初始化,
    虚拟串口_接口控制,
    虚拟串口_接口接收
};

/** PC 端串口参数（PC 端可改，本工程不依赖它） */
static USBD_CDC_LineCodingTypeDef 虚拟串口_行编码 =
{
    虚拟串口_默认波特率,    /* bitrate     */
    0x00U,                  /* 1 个停止位  */
    0x00U,                  /* 无校验      */
    0x08U                   /* 8 位数据    */
};

/** 发送环形缓冲：主循环写（发送写），SysTick 读（发送读） */
static volatile uint16_t 发送读 = 0U;
static volatile uint16_t 发送写 = 0U;
static uint8_t          发送缓冲[虚拟串口_发送缓冲大小];

/** USB 正在发送的那一包数据。传输期间这块内存必须保持有效，不能复用 */
static uint8_t 发送暂存区[虚拟串口_发送块大小];

/** 1 = 上一包还没发完（由 USB 中断清零） */
static volatile uint8_t 发送忙 = 0U;

/** 1 = 电脑已打开并配置好串口 */
static volatile uint8_t 已连接 = 0U;

/** 接收回调（电脑 -> 单片机） */
static void (*虚拟串口_接收回调)(const uint8_t *数据, uint32_t 长度) = NULL;

/* =========================== 描述符（设备级） ============================== */
/* 注意：CDC 的"配置描述符"由中间件 usbd_cdc.c 自带（USBD_CDC_CfgFSDesc），
   这里只需要提供设备描述符和各字符串描述符 */

/** STM32F1 的 96 位唯一 ID 基地址，用于生成 USB 序列号 */
#define 虚拟串口_UID地址    0x1FFFF7E8UL

static uint8_t *虚拟串口_设备描述符(USBD_SpeedTypeDef 速度, uint16_t *长度);
static uint8_t *虚拟串口_语言ID描述符(USBD_SpeedTypeDef 速度, uint16_t *长度);
static uint8_t *虚拟串口_厂商描述符(USBD_SpeedTypeDef 速度, uint16_t *长度);
static uint8_t *虚拟串口_产品描述符(USBD_SpeedTypeDef 速度, uint16_t *长度);
static uint8_t *虚拟串口_序列号描述符(USBD_SpeedTypeDef 速度, uint16_t *长度);
static uint8_t *虚拟串口_配置描述符(USBD_SpeedTypeDef 速度, uint16_t *长度);
static uint8_t *虚拟串口_接口描述符(USBD_SpeedTypeDef 速度, uint16_t *长度);

static USBD_DescriptorsTypeDef 虚拟串口_描述符表 =
{
    虚拟串口_设备描述符,
    虚拟串口_语言ID描述符,
    虚拟串口_厂商描述符,
    虚拟串口_产品描述符,
    虚拟串口_序列号描述符,
    虚拟串口_配置描述符,
    虚拟串口_接口描述符,
};

/** USB 标准设备描述符（18 字节） */
__ALIGN_BEGIN static uint8_t 设备描述符[USB_LEN_DEV_DESC] __ALIGN_END =
{
    0x12,                           /* bLength              */
    USB_DESC_TYPE_DEVICE,           /* bDescriptorType      */
    0x00,                           /* bcdUSB = 2.00        */
    0x02,
    0x02,                           /* bDeviceClass    = CDC    */
    0x02,                           /* bDeviceSubClass = ACM    */
    0x00,                           /* bDeviceProtocol = 无    */
    USB_MAX_EP0_SIZE,               /* bMaxPacketSize = 64     */
    LOBYTE(USBD_VID),               /* idVendor  低字节        */
    HIBYTE(USBD_VID),               /* idVendor  高字节        */
    LOBYTE(USBD_PID),               /* idProduct 低字节        */
    HIBYTE(USBD_PID),               /* idProduct 高字节        */
    0x00,                           /* bcdDevice = 2.00        */
    0x02,
    USBD_IDX_MFC_STR,               /* iManufacturer           */
    USBD_IDX_PRODUCT_STR,           /* iProduct                */
    USBD_IDX_SERIAL_STR,            /* iSerialNumber           */
    USBD_MAX_NUM_CONFIGURATION      /* bNumConfigurations      */
};

/** 语言 ID 字符串描述符（固定 4 字节） */
__ALIGN_BEGIN static uint8_t 语言ID描述符[USB_LEN_LANGID_STR_DESC] __ALIGN_END =
{
    USB_LEN_LANGID_STR_DESC,
    USB_DESC_TYPE_STRING,
    LOBYTE(USBD_LANGID_STRING),
    HIBYTE(USBD_LANGID_STRING)
};

/** 序列号描述符（内容由唯一 ID 生成） */
static uint8_t 序列号描述符[USB_SIZ_STRING_SERIAL] =
{
    USB_SIZ_STRING_SERIAL,
    USB_DESC_TYPE_STRING
};

/** 其余字符串描述符的临时拼装缓冲（USBD_GetString 往里写 UTF-16 结果） */
__ALIGN_BEGIN static uint8_t 字符串缓冲[USBD_MAX_STR_DESC_SIZ] __ALIGN_END;

/* ============================== 私有函数 =================================== */

/**
  * @brief  把 32 位数值按 16 进制转成 UTF-16LE 字符写入缓冲
  * @param  数值  待转换的值
  * @param  缓冲  目标缓冲
  * @param  位数  ASCII 字符个数
  */
static void 数值转Unicode(uint32_t 数值, uint8_t *缓冲, uint8_t 位数)
{
    uint8_t i;

    for (i = 0U; i < 位数; i++)
    {
        uint8_t 半字节 = (uint8_t)((数值 >> 28) & 0x0FU);

        缓冲[2U * i]      = (半字节 < 0x0AU) ? (uint8_t)(半字节 + '0')
                                             : (uint8_t)(半字节 + 'A' - 10U);
        缓冲[2U * i + 1U] = 0U;
        数值 = 数值 << 4;
    }
}

/**
  * @brief  用芯片唯一 ID 生成序列号字符串，避免多块板子在同一台电脑上撞号
  */
static void 生成序列号(void)
{
    uint32_t 低 = *(volatile uint32_t *)(虚拟串口_UID地址);
    uint32_t 中 = *(volatile uint32_t *)(虚拟串口_UID地址 + 4UL);
    uint32_t 高 = *(volatile uint32_t *)(虚拟串口_UID地址 + 8UL);

    低 += 高;

    if (低 != 0U)
    {
        数值转Unicode(低, &序列号描述符[2], 8U);
        数值转Unicode(中, &序列号描述符[18], 4U);
    }
}

/* ========================= 描述符回调实现 ================================== */

static uint8_t *虚拟串口_设备描述符(USBD_SpeedTypeDef 速度, uint16_t *长度)
{
    (void)速度;
    *长度 = sizeof(设备描述符);
    return (uint8_t *)设备描述符;
}

static uint8_t *虚拟串口_语言ID描述符(USBD_SpeedTypeDef 速度, uint16_t *长度)
{
    (void)速度;
    *长度 = sizeof(语言ID描述符);
    return (uint8_t *)语言ID描述符;
}

static uint8_t *虚拟串口_厂商描述符(USBD_SpeedTypeDef 速度, uint16_t *长度)
{
    (void)速度;
    USBD_GetString((uint8_t *)USBD_MANUFACTURER_STRING, 字符串缓冲, 长度);
    return 字符串缓冲;
}

static uint8_t *虚拟串口_产品描述符(USBD_SpeedTypeDef 速度, uint16_t *长度)
{
    (void)速度;
    USBD_GetString((uint8_t *)USBD_PRODUCT_STRING_FS, 字符串缓冲, 长度);
    return 字符串缓冲;
}

static uint8_t *虚拟串口_序列号描述符(USBD_SpeedTypeDef 速度, uint16_t *长度)
{
    (void)速度;
    *长度 = USB_SIZ_STRING_SERIAL;
    生成序列号();
    return (uint8_t *)序列号描述符;
}

static uint8_t *虚拟串口_配置描述符(USBD_SpeedTypeDef 速度, uint16_t *长度)
{
    (void)速度;
    USBD_GetString((uint8_t *)USBD_CONFIGURATION_STRING_FS, 字符串缓冲, 长度);
    return 字符串缓冲;
}

static uint8_t *虚拟串口_接口描述符(USBD_SpeedTypeDef 速度, uint16_t *长度)
{
    (void)速度;
    USBD_GetString((uint8_t *)USBD_INTERFACE_STRING_FS, 字符串缓冲, 长度);
    return 字符串缓冲;
}

/* ========================== CDC 接口回调实现 =============================== */

static int8_t 虚拟串口_接口初始化(void)
{
    /* USB 复位/配置完成时被调用，此时主机已经准备好收数据 */
    发送读  = 发送写;
    发送忙  = 0U;
    已连接  = 1U;
    return (int8_t)USBD_OK;
}

static int8_t 虚拟串口_接口反初始化(void)
{
    已连接 = 0U;
    发送忙 = 0U;
    return (int8_t)USBD_OK;
}

static int8_t 虚拟串口_接口控制(uint8_t 命令, uint8_t *数据, uint16_t 长度)
{
    (void)长度;

    switch (命令)
    {
        case CDC_SET_LINE_CODING:
            虚拟串口_行编码.bitrate    = (uint32_t)数据[0]
                                       | ((uint32_t)数据[1] << 8)
                                       | ((uint32_t)数据[2] << 16)
                                       | ((uint32_t)数据[3] << 24);
            虚拟串口_行编码.format     = 数据[4];
            虚拟串口_行编码.paritytype = 数据[5];
            虚拟串口_行编码.datatype   = 数据[6];
            break;

        case CDC_GET_LINE_CODING:
            数据[0] = (uint8_t)(虚拟串口_行编码.bitrate);
            数据[1] = (uint8_t)(虚拟串口_行编码.bitrate >> 8);
            数据[2] = (uint8_t)(虚拟串口_行编码.bitrate >> 16);
            数据[3] = (uint8_t)(虚拟串口_行编码.bitrate >> 24);
            数据[4] = 虚拟串口_行编码.format;
            数据[5] = 虚拟串口_行编码.paritytype;
            数据[6] = 虚拟串口_行编码.datatype;
            break;

        case CDC_SET_CONTROL_LINE_STATE:
            /* 电脑打开/关闭串口时会来这里，可用来判断终端是否已连接 */
            break;

        case CDC_SEND_BREAK:
            break;

        default:
            break;
    }

    return (int8_t)USBD_OK;
}

static int8_t 虚拟串口_接口接收(uint8_t *数据, uint32_t *长度)
{
    if ((虚拟串口_接收回调 != NULL) && (数据 != NULL) && (长度 != NULL) && (*长度 > 0U))
    {
        虚拟串口_接收回调(数据, *长度);
    }

    /* 重新武装 OUT 端点，准备接收下一包 */
    (void)USBD_CDC_ReceivePacket(&虚拟串口_设备);

    return (int8_t)USBD_OK;
}

/* ============================== 公开接口 =================================== */

void 虚拟串口_初始化(void)
{
    if (USBD_Init(&虚拟串口_设备, &虚拟串口_描述符表, 0U) != USBD_OK) {
        Error_Handler();
    }

    if (USBD_RegisterClass(&虚拟串口_设备, &USBD_CDC) != USBD_OK) {
        Error_Handler();
    }

    if (USBD_CDC_RegisterInterface(&虚拟串口_设备, &虚拟串口_CDC接口) != USBD_OK) {
        Error_Handler();
    }

    if (USBD_Start(&虚拟串口_设备) != USBD_OK) {
        Error_Handler();
    }
}

uint16_t 虚拟串口_发送(const uint8_t *数据, uint16_t 长度)
{
    uint16_t 已存入 = 0U;

    if (数据 == NULL) {
        return 0U;
    }

    while (已存入 < 长度)
    {
        uint16_t 下一个写位置 = (uint16_t)((发送写 + 1U) % 虚拟串口_发送缓冲大小);

        if (下一个写位置 == 发送读) {
            break;              /* 缓冲满：丢弃剩余数据，绝不阻塞调用者 */
        }

        发送缓冲[发送写] = 数据[已存入];
        发送写 = 下一个写位置;
        已存入++;
    }

    return 已存入;
}

void 虚拟串口_发送完成(void)
{
    /* 在 USB 中断里被调用，只清标志；下一包由 虚拟串口_轮询() 取走 */
    发送忙 = 0U;
}

void 虚拟串口_轮询(void)
{
    uint16_t        块长度 = 0U;
    uint16_t        读位置;
    uint32_t        原中断屏蔽;

    if (发送忙 != 0U) {
        return;                 /* 上一包还在总线上 */
    }

    if (发送读 == 发送写) {
        return;                 /* 没有待发数据 */
    }

    if (已连接 == 0U)
    {
        /* 电脑没在收：直接丢弃，避免缓冲长期占满 */
        发送读 = 发送写;
        return;
    }

    /* 从环形缓冲取一段，先不动读指针，等真正启动成功后再移动 */
    读位置 = 发送读;
    while ((块长度 < 虚拟串口_发送块大小) && (读位置 != 发送写))
    {
        发送暂存区[块长度] = 发送缓冲[读位置];
        块长度++;
        读位置 = (uint16_t)((读位置 + 1U) % 虚拟串口_发送缓冲大小);
    }

    /* USB 中断里也会操作端点寄存器，这里短暂关中断做互斥（约 2 µs） */
    原中断屏蔽 = __get_PRIMASK();
    __disable_irq();

    if ((USBD_CDC_SetTxBuffer(&虚拟串口_设备, 发送暂存区, 块长度) == USBD_OK)
        && (USBD_CDC_TransmitPacket(&虚拟串口_设备) == USBD_OK))
    {
        发送忙 = 1U;
        发送读 = 读位置;
    }

    __set_PRIMASK(原中断屏蔽);
}

uint8_t 虚拟串口_已连接(void)
{
    return 已连接;
}

void 虚拟串口_设置接收回调(void (*回调)(const uint8_t *数据, uint32_t 长度))
{
    虚拟串口_接收回调 = 回调;
}
