/**
 ******************************************************************************
 * @file    usbd_conf.c
 * @brief   USB Device 底层对接层：把 STM32 HAL 的 PCD 驱动接到 USB Device 协议栈
 *
 *  ⚠ 本文件所有函数名都是"被按名字调用"的，不能改：
 *      USBD_LL_xxx   —— 由 usbd_core.c / usbd_ctlreq.c 调用
 *      HAL_PCD_xxx   —— HAL 的弱符号回调，改名后中断里的 USB 状态机就断了
 *      因此本文件是全工程唯一必须保持英文命名的模块。
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usbd_conf.h"
#include "usbd_core.h"
#include "usbd_cdc.h"
#include "vcp.h"

/* ============================== 全局变量 =================================== */

/** USB 外设句柄（stm32f1xx_it.c 的中断服务程序要用） */
PCD_HandleTypeDef hpcd_USB_FS;

/* =========================== 底层驱动对接 ================================== */

/**
  * @brief  初始化 USB 底层驱动。由 USBD_Init() 调用。
  * @note   STM32F1 的 USB 是"传统" USB 外设（非 OTG），
  *         它的 512 字节 PMA（Packet Memory Area）必须手工分配给各端点，
  *         漏掉 HAL_PCDEx_PMAConfig() 会导致端点收发完全不工作，
  *         而且不会有任何编译警告 —— 这是 F1 上最容易踩的坑。
  */
USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef *pdev)
{
    /* 把底层句柄和协议栈句柄互相挂上 */
    hpcd_USB_FS.pData = pdev;
    pdev->pData = &hpcd_USB_FS;

    hpcd_USB_FS.Instance                     = USB;
    hpcd_USB_FS.Init.dev_endpoints           = 8U;
    hpcd_USB_FS.Init.speed                   = PCD_SPEED_FULL;
    hpcd_USB_FS.Init.low_power_enable        = DISABLE;
    hpcd_USB_FS.Init.lpm_enable              = DISABLE;
    hpcd_USB_FS.Init.battery_charging_enable = DISABLE;

    if (HAL_PCD_Init(&hpcd_USB_FS) != HAL_OK) {
        Error_Handler();
    }

    /* PMA 分配表（与 ST 官方 CDC 例程一致，共 0x120 字节 ≤ 512）
         端点        地址    大小
         EP0  OUT    0x18    64 B
         EP0  IN     0x58    64 B
         CDC  IN     0x98    64 B
         CDC  OUT    0xD8    64 B
         CDC  CMD    0x118    8 B   */
    (void)HAL_PCDEx_PMAConfig(&hpcd_USB_FS, 0x00,       PCD_SNG_BUF, 0x18U);
    (void)HAL_PCDEx_PMAConfig(&hpcd_USB_FS, 0x80,       PCD_SNG_BUF, 0x58U);
    (void)HAL_PCDEx_PMAConfig(&hpcd_USB_FS, CDC_IN_EP,  PCD_SNG_BUF, 0x98U);
    (void)HAL_PCDEx_PMAConfig(&hpcd_USB_FS, CDC_OUT_EP, PCD_SNG_BUF, 0xD8U);
    (void)HAL_PCDEx_PMAConfig(&hpcd_USB_FS, CDC_CMD_EP, PCD_SNG_BUF, 0x118U);

    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef *pdev)
{
    (void)HAL_PCD_DeInit(pdev->pData);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef *pdev)
{
    (void)HAL_PCD_Start(pdev->pData);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef *pdev)
{
    (void)HAL_PCD_Stop(pdev->pData);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                  uint8_t ep_type, uint16_t ep_mps)
{
    (void)HAL_PCD_EP_Open(pdev->pData, ep_addr, ep_mps, ep_type);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    (void)HAL_PCD_EP_Close(pdev->pData, ep_addr);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    (void)HAL_PCD_EP_Flush(pdev->pData, ep_addr);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    (void)HAL_PCD_EP_SetStall(pdev->pData, ep_addr);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    (void)HAL_PCD_EP_ClrStall(pdev->pData, ep_addr);
    return USBD_OK;
}

uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    PCD_HandleTypeDef *hpcd = (PCD_HandleTypeDef *)pdev->pData;

    if ((ep_addr & 0x80U) == 0x80U) {
        return hpcd->IN_ep[ep_addr & 0x7FU].is_stall;
    }

    return hpcd->OUT_ep[ep_addr & 0x7FU].is_stall;
}

USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef *pdev, uint8_t dev_addr)
{
    (void)HAL_PCD_SetAddress(pdev->pData, dev_addr);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                    uint8_t *pbuf, uint16_t size)
{
    (void)HAL_PCD_EP_Transmit(pdev->pData, ep_addr, pbuf, size);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                          uint8_t *pbuf, uint16_t size)
{
    (void)HAL_PCD_EP_Receive(pdev->pData, ep_addr, pbuf, size);
    return USBD_OK;
}

uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    /* CDC 类靠这个函数拿"实际收到多少字节"，返回 0 会导致收到的数据长度错乱 */
    return HAL_PCD_EP_GetRxCount((PCD_HandleTypeDef *)pdev->pData, ep_addr);
}

void USBD_LL_Delay(uint32_t Delay)
{
    HAL_Delay(Delay);
}

/* ====================== HAL PCD 回调（转交给协议栈） ======================= */

void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd)
{
    (void)USBD_LL_SetupStage((USBD_HandleTypeDef *)hpcd->pData, (uint8_t *)hpcd->Setup);
}

void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    (void)USBD_LL_DataOutStage((USBD_HandleTypeDef *)hpcd->pData, epnum,
                               hpcd->OUT_ep[epnum].xfer_buff);
}

void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    (void)USBD_LL_DataInStage((USBD_HandleTypeDef *)hpcd->pData, epnum,
                              hpcd->IN_ep[epnum].xfer_buff);

    /* 本包已经发完，通知虚拟串口模块可以继续取下一包 */
    虚拟串口_发送完成();
}

void HAL_PCD_SOFCallback(PCD_HandleTypeDef *hpcd)
{
    (void)USBD_LL_SOF((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ResetCallback(PCD_HandleTypeDef *hpcd)
{
    (void)USBD_LL_SetSpeed((USBD_HandleTypeDef *)hpcd->pData, USBD_SPEED_FULL);
    (void)USBD_LL_Reset((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *hpcd)
{
    (void)USBD_LL_Suspend((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *hpcd)
{
    (void)USBD_LL_Resume((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    (void)USBD_LL_IsoOUTIncomplete((USBD_HandleTypeDef *)hpcd->pData, epnum);
}

void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    (void)USBD_LL_IsoINIncomplete((USBD_HandleTypeDef *)hpcd->pData, epnum);
}

void HAL_PCD_ConnectCallback(PCD_HandleTypeDef *hpcd)
{
    (void)USBD_LL_DevConnected((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *hpcd)
{
    (void)USBD_LL_DevDisconnected((USBD_HandleTypeDef *)hpcd->pData);
}

/* ============================== MSP 初始化 ================================= */

/**
  * @brief  USB 外设底层初始化，由 HAL_PCD_Init() 自动调用
  */
void HAL_PCD_MspInit(PCD_HandleTypeDef *hpcd)
{
    if (hpcd->Instance == USB)
    {
        __HAL_RCC_USB_CLK_ENABLE();

        /* F1 上 USB 与 CAN 共用中断向量，设备模式只用低优先级那个。
           优先级给 0（最高），保证 USB 时序不被其它中断拖慢 */
        HAL_NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 0, 0);
        HAL_NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
    }
}

/**
  * @brief  USB 外设底层反初始化
  */
void HAL_PCD_MspDeInit(PCD_HandleTypeDef *hpcd)
{
    if (hpcd->Instance == USB)
    {
        __HAL_RCC_USB_CLK_DISABLE();

        HAL_NVIC_DisableIRQ(USB_LP_CAN1_RX0_IRQn);
    }
}
