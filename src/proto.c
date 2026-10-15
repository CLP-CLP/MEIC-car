/**
 ******************************************************************************
 * @file    proto.c
 * @brief   协议接收端硬件侧：USART1（PA9 = TX / PA10 = RX，115200 8N1）
 *
 *  职责
 *      1. 初始化 USART1，开中断，逐字节接收（HAL_UART_Receive_IT，每次收 1 字节）
 *      2. 接收完成回调里把字节喂给解析器（proto_parse.c），并**立刻重新武装**
 *      3. 错误回调里清 ORE 并重新武装（漏掉会表现为"跑一会儿就再也收不到"）
 *      4. 给主循环提供带临界区的结果读取接口
 *
 *  说明
 *      HAL_UART_MspInit / USART1_IRQHandler 都放在本模块里，是为了让这个模块
 *      自成一体、不必改 stm32f1xx_it.c 和 stm32f1xx_hal_msp.c。
 *      如果你们习惯把中断都写在 stm32f1xx_it.c，把那 4 行搬过去即可。
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "proto.h"

/* ============================== 私有变量 =================================== */

static UART_HandleTypeDef 协议_UART;
static uint8_t            rx_byte;

/** 上一帧通过校验的时间戳，0 = 还没收到过 */
static volatile uint32_t 收帧时刻 = 0U;

/* 硬件层诊断计数：跟协议解析无关，纯看串口外设自己出了什么事 */
static volatile uint32_t 串口错误数 = 0U;
static volatile uint32_t 溢出错误数 = 0U;
static volatile uint32_t 喂狗救活数 = 0U;

/* ============================== MSP 初始化 ================================= */

/**
  * @brief  USART1 底层初始化，由 HAL_UART_Init() 自动调用
  */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (huart->Instance == USART1)
    {
        __HAL_RCC_USART1_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();

        /* PA9 -> USART1_TX，复用推挽输出 */
        GPIO_InitStruct.Pin   = GPIO_PIN_9;
        GPIO_InitStruct.Mode  = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull  = GPIO_NOPULL;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        /* PA10 -> USART1_RX，浮空输入（OpenMV 持续发送，不需要上拉） */
        GPIO_InitStruct.Pin  = GPIO_PIN_10;
        GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        /* 字节是秒级到达的，优先级不用高；给 3（高于 SysTick 的 15，低于 USB 的 0） */
        HAL_NVIC_SetPriority(USART1_IRQn, 3, 0);
        HAL_NVIC_EnableIRQ(USART1_IRQn);
    }
}

/* ============================== 初始化 ===================================== */

void 协议_初始化(void)
{
    协议_解析清零();
    收帧时刻   = 0U;
    串口错误数 = 0U;
    溢出错误数 = 0U;
    喂狗救活数 = 0U;

    协议_UART.Instance          = USART1;
    协议_UART.Init.BaudRate     = 协议_波特率;
    协议_UART.Init.WordLength   = UART_WORDLENGTH_8B;
    协议_UART.Init.StopBits     = UART_STOPBITS_1;
    协议_UART.Init.Parity       = UART_PARITY_NONE;
    协议_UART.Init.Mode         = UART_MODE_TX_RX;
    协议_UART.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    协议_UART.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&协议_UART) != HAL_OK) {
        Error_Handler();
    }

    /* 逐字节中断接收；不需要 DMA，也不需要阻塞式接收 */
    if (HAL_UART_Receive_IT(&协议_UART, &rx_byte, 1U) != HAL_OK) {
        Error_Handler();
    }
}

/* ============================== 中断 ======================================= */

/**
  * @brief  USART1 全局中断（覆盖启动文件里的弱定义）
  */
void USART1_IRQHandler(void)
{
    HAL_UART_IRQHandler(&协议_UART);
}

/**
  * @brief  收到 1 个字节：喂给解析器，然后立刻重新武装
  */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    uint32_t 之前好帧;

    if (huart->Instance == USART1)
    {
        之前好帧 = 协议_解析好帧数();

        协议_解析喂字节(rx_byte);

        if (协议_解析好帧数() != 之前好帧) {
            收帧时刻 = HAL_GetTick();       /* 这一帧通过了校验 */
        }

        (void)HAL_UART_Receive_IT(huart, &rx_byte, 1U);   /* 必须重新武装 */
    }
}

/**
  * @brief  串口错误（溢出 ORE 等）：清标志并重新武装，否则会永久收不到
  */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        串口错误数++;
        if ((huart->ErrorCode & HAL_UART_ERROR_ORE) != 0U) {
            溢出错误数++;
        }

        __HAL_UART_CLEAR_OREFLAG(huart);
        (void)HAL_UART_Receive_IT(huart, &rx_byte, 1U);
    }
}

/* ========================= 硬件层诊断（排线用） ============================ */

uint32_t 协议_串口错误数(void)  { return 串口错误数; }
uint32_t 协议_溢出错误数(void)  { return 溢出错误数; }
uint32_t 协议_喂狗救活数(void)  { return 喂狗救活数; }

void 协议_喂狗(void)
{
    /* 正常情况：串口一直在等字节（BUSY_RX），这里直接跳过 */
    if (协议_UART.RxState != HAL_UART_STATE_BUSY_RX)
    {
        喂狗救活数++;
        __HAL_UART_CLEAR_OREFLAG(&协议_UART);

        if (HAL_UART_Receive_IT(&协议_UART, &rx_byte, 1U) != HAL_OK) {
            /* 连重新武装都失败（被锁住）：把串口整个重来一遍 */
            (void)HAL_UART_DeInit(&协议_UART);
            (void)HAL_UART_Init(&协议_UART);
            (void)HAL_UART_Receive_IT(&协议_UART, &rx_byte, 1U);
        }
    }
}

uint32_t 协议_线路活动(uint8_t *末电平)
{
    uint32_t i;
    uint32_t 边沿 = 0U;
    uint8_t  上次 = ((GPIOA->IDR & GPIO_PIN_10) != 0U) ? 1U : 0U;
    uint8_t  现在;

    /* PA10 是浮空输入时也能直接读 IDR：这里完全不经过串口外设，
       所以"线上到底动没动"和"串口收没收到"是两个独立的证据 */
    for (i = 0U; i < 协议_线路采样点数; i++)
    {
        现在 = ((GPIOA->IDR & GPIO_PIN_10) != 0U) ? 1U : 0U;

        if (现在 != 上次) {
            边沿++;
            上次 = 现在;
        }
    }

    if (末电平 != 0) {
        *末电平 = 上次;
    }

    return 边沿;
}

/**
  * @brief  把 PA10 重新配成输入，指定内部上拉/下拉（改完记得恢复成 NOPULL）
  */
static void PA10_挂电阻(uint32_t 上下拉)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    GPIO_InitStruct.Pin   = GPIO_PIN_10;
    GPIO_InitStruct.Mode  = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull  = 上下拉;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

void 协议_线路探测(uint8_t *上拉电平, uint8_t *下拉电平)
{
    uint8_t 上 = 0U;
    uint8_t 下 = 0U;

    /* 挂内部上拉（约 40 kΩ）：能把这根线拉起来，就说明线上没有强下拉 */
    PA10_挂电阻(GPIO_PULLUP);
    HAL_Delay(1U);
    上 = ((GPIOA->IDR & GPIO_PIN_10) != 0U) ? 1U : 0U;

    /* 再挂内部下拉：如果照样读到 0，说明线上也没人驱动高 */
    PA10_挂电阻(GPIO_PULLDOWN);
    HAL_Delay(1U);
    下 = ((GPIOA->IDR & GPIO_PIN_10) != 0U) ? 1U : 0U;

    /* 恢复成串口接收的正常配置（浮空输入） */
    PA10_挂电阻(GPIO_NOPULL);

    if (上拉电平 != 0) { *上拉电平 = 上; }
    if (下拉电平 != 0) { *下拉电平 = 下; }
}

/* ============================== 对外接口 =================================== */

void 协议_取结果(协议_结果 *结果)
{
    uint32_t 原中断;

    if (结果 == 0) {
        return;
    }

    /* 解析器在中断里跑，这里做一次极短的临界区（只有几条指令） */
    原中断 = __get_PRIMASK();
    __disable_irq();
    协议_解析取结果(结果);
    __set_PRIMASK(原中断);
}

uint8_t 协议_有卡片(void)
{
    协议_结果 结果;

    协议_取结果(&结果);

    return ((结果.有效 != 0U) && (结果.状态 == 1U)) ? 1U : 0U;
}

uint8_t 协议_通信正常(void)
{
    uint32_t 时刻 = 收帧时刻;

    if (时刻 == 0U) {
        return 0U;                      /* 还没收到过任何有效帧 */
    }

    return ((uint32_t)(HAL_GetTick() - 时刻) < 协议_掉线毫秒) ? 1U : 0U;
}

uint32_t 协议_上次收帧时刻(void)
{
    return 收帧时刻;
}

/* ==================== 给程序逻辑用（不打印，直接调用）==================== */

uint8_t 协议_取卡面(void)
{
    协议_结果 结果;

    /* 链路断了就当没有卡片：宁可当成"没认出来"，也不能拿过期结果去跑车 */
    if (协议_通信正常() == 0U) {
        return 0U;
    }

    协议_取结果(&结果);

    return (结果.状态 == 1U) ? 结果.罗马数字 : 0U;
}

uint8_t 协议_取库位(void)
{
    协议_结果 结果;

    if (协议_通信正常() == 0U) {
        return 0U;
    }

    协议_取结果(&结果);

    return (结果.状态 == 1U) ? 结果.停车点 : 0U;
}

uint8_t 协议_取颜色(void)
{
    协议_结果 结果;

    if (协议_通信正常() == 0U) {
        return 0U;
    }

    协议_取结果(&结果);

    return (结果.状态 == 1U) ? 结果.颜色 : 0U;
}

uint8_t 协议_取形状(void)
{
    协议_结果 结果;

    if (协议_通信正常() == 0U) {
        return 0U;
    }

    协议_取结果(&结果);

    return (结果.状态 == 1U) ? 结果.形状 : 0U;
}

uint8_t 协议_等待卡片(uint32_t 超时毫秒, 协议_结果 *结果)
{
    uint32_t 开始时刻 = HAL_GetTick();

    while (1)
    {
        /* 必须是"链路正常"且"有卡片"：单看有卡片会立刻返回上一轮的旧结果 */
        if ((协议_通信正常() != 0U) && (协议_有卡片() != 0U))
        {
            if (结果 != 0) {
                协议_取结果(结果);
            }
            return 1U;
        }

        if ((超时毫秒 != 0U)
            && ((uint32_t)(HAL_GetTick() - 开始时刻) >= 超时毫秒)) {
            return 0U;
        }

        协议_喂狗();
        HAL_Delay(5U);
    }
}

uint32_t 协议_好帧数(void)
{
    return 协议_解析好帧数();
}

uint32_t 协议_错帧数(void)
{
    return 协议_解析错帧数();
}

uint32_t 协议_字节总数(void)
{
    return 协议_解析字节总数();
}

void 协议_最近字节(uint8_t *缓冲)
{
    协议_解析最近字节(缓冲);
}

/* ============================== 回环自检 =================================== */

/**
  * @brief  PA9 <-> PA10 短接时，自己发自己收，用来把"F103 收端"和"OpenMV/线"分开
  */
uint32_t 协议_回环自检(void)
{
    /* 和 PROTOCOL.md 第 5 节一样的三帧 STATUS + 一帧 TEXT */
    static const uint8_t 向量[4][9] = {
        {0xAA,0x55,0x01,0x04,0x00,0x00,0x00,0x00,0x05},
        {0xAA,0x55,0x01,0x04,0x01,0x41,0x01,0x07,0x43},
        {0xAA,0x55,0x01,0x04,0x01,0x44,0x07,0x0A,0x4D},
        {0xAA,0x55,0x10,0x02,0x41,0x0A,0x59,0x00,0x00}
    };
    static const uint8_t 长度[4] = {9U, 9U, 9U, 7U};

    uint32_t 存下收帧时刻 = 收帧时刻;      /* 回环帧不该冒充"真的收到 OpenMV 了" */
    uint32_t 收回帧数;
    uint32_t i;

    协议_解析清零();

    for (i = 0U; i < 4U; i++) {
        (void)HAL_UART_Transmit(&协议_UART, (uint8_t *)(const void *)向量[i],
                                长度[i], 100U);
    }

    HAL_Delay(20U);                         /* 34 字节 @115200 只要 3 ms，留足余量 */

    收回帧数 = 协议_解析好帧数();            /* TEXT 不算好帧，所以期望 3 */

    协议_解析清零();
    收帧时刻 = 存下收帧时刻;

    return 收回帧数;
}

/* ============================== 自检 ======================================= */

/**
  * @brief  用 PROTOCOL.md 第 5 节的报文向量自测解析器（不用接 OpenMV）
  * @retval 1 = 全部通过
  */
uint8_t 协议_自检(void)
{
    /* PROTOCOL.md 第 5 节给出的真实报文 */
    static const uint8_t 向量_无结果[] = {0xAA,0x55,0x01,0x04,0x00,0x00,0x00,0x00,0x05};
    static const uint8_t 向量_A[]      = {0xAA,0x55,0x01,0x04,0x01,0x41,0x01,0x07,0x43};
    static const uint8_t 向量_D[]      = {0xAA,0x55,0x01,0x04,0x01,0x44,0x07,0x0A,0x4D};
    static const uint8_t 向量_文本[]   = {0xAA,0x55,0x10,0x02,0x41,0x0A,0x59};

    /* 额外构造的边界用例 */
    static const uint8_t 向量_坏校验[] = {0xAA,0x55,0x01,0x04,0x01,0x41,0x01,0x07,0x00};
    static const uint8_t 向量_丢字节[] = {0xAA,0xAA,0x55,0x01,0x04,0x01,0x41,0x01,0x07,0x43};

    协议_结果 结果;
    uint32_t 错帧基准;
    uint8_t i;

    协议_解析清零();

    /* 1. 没有卡片：state = 0 */
    for (i = 0U; i < (uint8_t)sizeof(向量_无结果); i++) 协议_解析喂字节(向量_无结果[i]);
    协议_解析取结果(&结果);
    if (结果.有效 != 1U || 结果.状态 != 0U || 结果.停车点 != 0U
        || 结果.罗马数字 != 0U || 结果.序号 != 0U) {
        return 0U;
    }
    if (协议_解析好帧数() != 1U) {
        return 0U;
    }

    /* 2. 识别到 A（卡面 I） */
    for (i = 0U; i < (uint8_t)sizeof(向量_A); i++) 协议_解析喂字节(向量_A[i]);
    协议_解析取结果(&结果);
    if (结果.状态 != 1U || 结果.停车点 != (uint8_t)'A'
        || 结果.罗马数字 != 1U || 结果.序号 != 7U) {
        return 0U;
    }

    /* 3. 识别到 D（卡面 VII） */
    for (i = 0U; i < (uint8_t)sizeof(向量_D); i++) 协议_解析喂字节(向量_D[i]);
    协议_解析取结果(&结果);
    if (结果.状态 != 1U || 结果.停车点 != (uint8_t)'D'
        || 结果.罗马数字 != 7U || 结果.序号 != 10U) {
        return 0U;
    }

    /* 4. 校验错的帧：必须丢弃，且"最新结果"保持 D 不变 */
    错帧基准 = 协议_解析错帧数();
    for (i = 0U; i < (uint8_t)sizeof(向量_坏校验); i++) 协议_解析喂字节(向量_坏校验[i]);
    协议_解析取结果(&结果);
    if (结果.停车点 != (uint8_t)'D') {
        return 0U;
    }
    if (协议_解析错帧数() != (错帧基准 + 1U)) {
        return 0U;
    }

    /* 5. 未知类型（TEXT）不算错误，也不影响结果 */
    for (i = 0U; i < (uint8_t)sizeof(向量_文本); i++) 协议_解析喂字节(向量_文本[i]);
    协议_解析取结果(&结果);
    if (结果.停车点 != (uint8_t)'D') {
        return 0U;
    }
    if (协议_解析错帧数() != (错帧基准 + 1U)) {
        return 0U;
    }

    /* 6. 线上丢了一个字节（AA AA 55 ...）后仍要能解出正确的帧 */
    for (i = 0U; i < (uint8_t)sizeof(向量_丢字节); i++) 协议_解析喂字节(向量_丢字节[i]);
    协议_解析取结果(&结果);
    if (结果.状态 != 1U || 结果.停车点 != (uint8_t)'A'
        || 结果.罗马数字 != 1U || 结果.序号 != 7U) {
        return 0U;
    }

    return 1U;
}