/**
 ******************************************************************************
 * @file    button.c
 * @brief   三键输入：消抖 + 短按 / 长按识别
 *
 *  上键(PB12) / 下键(PB14) : 短按 -> BTN_UP / BTN_DOWN
 *  确认键(PB13)           : 2 秒内松手 -> BTN_SEL
 *                           按住满 2 秒 -> BTN_BACK
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "button.h"

/* ============================== 参数 ======================================= */

#define DEBOUNCE_MS         20U         /* 消抖时间（毫秒） */

/* ============================== 单键状态 =================================== */

typedef struct
{
    uint16_t    pin;
    uint8_t     stable;                 /* 消抖后的稳定状态：1 = 按下 */
    uint8_t     last_raw;
    uint32_t    change_tick;            /* 上次电平变化时刻 */
    uint32_t    press_tick;             /* 按下时刻（长按计时） */
    uint8_t     long_fired;             /* 长按事件是否已报 */
} _key_t;

static _key_t _keys[3];

/* ============================== 初始化 ===================================== */

void 按键_初始化(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitStruct.Pin   = BTN_UP_PIN | BTN_DOWN_PIN | BTN_SEL_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull  = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BTN_PORT, &GPIO_InitStruct);

    _keys[0].pin = BTN_UP_PIN;
    _keys[1].pin = BTN_DOWN_PIN;
    _keys[2].pin = BTN_SEL_PIN;
}

/* ============================== 事件读取 =================================== */

/** 读单个按键原始电平：1 = 按下 */
static uint8_t _read_raw(uint16_t pin)
{
    return (HAL_GPIO_ReadPin(BTN_PORT, pin) == GPIO_PIN_RESET) ? 1U : 0U;
}

/**
  * @brief  处理单个按键的状态机，返回事件
  * @retval 0=无事件, 1=短按, 2=长按
  */
static uint8_t _key_process(_key_t *k)
{
    uint8_t  raw = _read_raw(k->pin);
    uint32_t now = HAL_GetTick();

    if (raw != k->last_raw)
    {
        k->last_raw   = raw;
        k->change_tick = now;
        return 0;
    }

    if ((uint32_t)(now - k->change_tick) < DEBOUNCE_MS) {
        return 0;
    }

    /* 稳定状态没变：检查长按 */
    if (raw == k->stable)
    {
        if ((k->stable != 0U) && (k->long_fired == 0U)
            && ((uint32_t)(now - k->press_tick) >= BTN_LONG_MS))
        {
            k->long_fired = 1U;
            return 2;                       /* 长按 */
        }
        return 0;
    }

    /* 稳定状态变了 */
    {
        uint8_t just_pressed  = raw & (uint8_t)(~k->stable);
        uint8_t just_released = k->stable & (uint8_t)(~raw);

        k->stable = raw;

        if (just_pressed != 0U)
        {
            k->press_tick = now;
            k->long_fired = 0U;
            return 0;
        }

        if (just_released != 0U)
        {
            if (k->long_fired == 0U) {
                return 1;                   /* 短按 */
            }
            k->long_fired = 0U;
            return 0;
        }
    }

    return 0;
}

btn_event_t 按键_取事件(void)
{
    uint8_t r;

    r = _key_process(&_keys[0]);
    if (r == 1) return BTN_UP;

    r = _key_process(&_keys[1]);
    if (r == 1) return BTN_DOWN;

    r = _key_process(&_keys[2]);
    if (r == 1) return BTN_SEL;
    if (r == 2) return BTN_BACK;

    return BTN_NONE;
}