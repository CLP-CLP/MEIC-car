/**
 ******************************************************************************
 * @file    motor.c
 * @brief   L298N 双直流电机驱动（方向 IO + PWM 调速）
 *
 *  PWM 参数
 *      TIM3 挂在 APB1 上，APB1 预分频 = 2，所以定时器时钟 = 72 MHz
 *      预分频 PSC = 0    -> 计数频率 72 MHz
 *      自动重装 ARR = 3599 -> PWM 频率 = 72 MHz / 3600 = 20 kHz
 *      占空比分辨率 3600 级（0.028%），完全够用
 *
 *  为什么方向用 IO、调速用 PWM（而不是 PWM 打方向脚）
 *      L298N 的 EN 脚才是使能/调速端，IN 脚只负责方向。
 *      这样 PWM 波形不经过方向逻辑，转向时不会有额外抖动。
 *
 *  死区补偿
 *      实测硬件占空比低于 65% 时电机不转，所以对外接口的 1~100 会线性
 *      映射到硬件 65~100（见 电机_转速）。这个 65 在 motor.h 里改。
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "motor.h"

/* ============================== 私有变量 =================================== */

/** PWM 定时器句柄（本模块私有） */
static TIM_HandleTypeDef htim3;

/** 方向引脚表：[电机编号][0=IN1, 1=IN2] */
static GPIO_TypeDef *const 电机_方向端口表[2][2] =
{
    { 电机_左_IN1_端口, 电机_左_IN2_端口 },
    { 电机_右_IN1_端口, 电机_右_IN2_端口 },
};

static const uint16_t 电机_方向引脚表[2][2] =
{
    { 电机_左_IN1_引脚, 电机_左_IN2_引脚 },
    { 电机_右_IN1_引脚, 电机_右_IN2_引脚 },
};

/** PWM 通道表，下标 = 电机_编号 */
static const uint32_t 电机_PWM通道表[2] =
{
    电机_左_PWM通道,
    电机_右_PWM通道,
};

/** 方向取反表，下标 = 电机_编号（1 = 正/反转对调，见 motor.h 的方向纠正） */
static const uint8_t 电机_方向取反表[2] =
{
    电机_左_方向取反,
    电机_右_方向取反,
};

/* ============================== 私有函数 =================================== */

/**
  * @brief  写方向脚
  * @param  哪台  电机编号
  * @param  方向  1 = 正转, -1 = 反转, 0 = 两脚同低（配合 PWM=0 即滑行）
  * @note   会先按 电机_x_方向取反 宏做纠正，所以这里的 1 一定对应
  *         车体上的"前进"，与接线顺序无关
  */
static void 电机_写方向(电机_编号 哪台, int8_t 方向)
{
    GPIO_PinState 第一脚 = GPIO_PIN_RESET;
    GPIO_PinState 第二脚 = GPIO_PIN_RESET;

    /* 方向纠正：接线/安装朝向不同导致正转反了，用宏翻转，不必重新焊线 */
    if (电机_方向取反表[哪台] != 0U) {
        方向 = (int8_t)(0 - 方向);          /* 1 <-> -1，0 仍是 0 */
    }

    if (方向 > 0) {
        第一脚 = GPIO_PIN_SET;          /* IN1=1, IN2=0 */
    } else if (方向 < 0) {
        第二脚 = GPIO_PIN_SET;          /* IN1=0, IN2=1 */
    }

    HAL_GPIO_WritePin(电机_方向端口表[哪台][0], 电机_方向引脚表[哪台][0], 第一脚);
    HAL_GPIO_WritePin(电机_方向端口表[哪台][1], 电机_方向引脚表[哪台][1], 第二脚);
}

/* ============================== 函数实现 =================================== */

void 电机_初始化(void)
{
    GPIO_InitTypeDef   GPIO_InitStruct = {0};
    TIM_OC_InitTypeDef sConfigOC       = {0};
    uint16_t           全部方向脚;

    全部方向脚 = (uint16_t)(电机_左_IN1_引脚 | 电机_左_IN2_引脚
                          | 电机_右_IN1_引脚 | 电机_右_IN2_引脚);

    /* ---- 1. 方向引脚：PB0 / PB1 / PB10 / PB11 推挽输出 ---- */
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* 先输出低电平，保证上电瞬间电机不会乱转 */
    HAL_GPIO_WritePin(电机_方向端口, 全部方向脚, GPIO_PIN_RESET);

    GPIO_InitStruct.Pin   = 全部方向脚;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(电机_方向端口, &GPIO_InitStruct);

    /* ---- 2. PWM：TIM3_CH1(PA6) / TIM3_CH2(PA7)，20 kHz ---- */
    htim3.Instance               = 电机_PWM定时器;
    htim3.Init.Prescaler         = 0U;                      /* 计数频率 72 MHz */
    htim3.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim3.Init.Period            = 电机_PWM自动重装值;      /* 72M / 3600 = 20 kHz */
    htim3.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

    /* HAL_TIM_PWM_Init() 内部会调用 HAL_TIM_PWM_MspInit()：
       使能 TIM3 时钟，并把 PA6 / PA7 配成复用推挽输出 */
    if (HAL_TIM_PWM_Init(&htim3) != HAL_OK) {
        Error_Handler();
    }

    sConfigOC.OCMode     = TIM_OCMODE_PWM1;
    sConfigOC.Pulse      = 0U;                  /* 上电占空比 0 */
    sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH; /* 高电平有效 */
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;

    if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, 电机_左_PWM通道) != HAL_OK) {
        Error_Handler();
    }
    if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, 电机_右_PWM通道) != HAL_OK) {
        Error_Handler();
    }

    if (HAL_TIM_PWM_Start(&htim3, 电机_左_PWM通道) != HAL_OK) {
        Error_Handler();
    }
    if (HAL_TIM_PWM_Start(&htim3, 电机_右_PWM通道) != HAL_OK) {
        Error_Handler();
    }

    电机_全部停止();
}

void 电机_转速(电机_编号 哪台, int16_t 占空比)
{
    uint32_t 硬件占空比;
    uint32_t 比较值;
    uint16_t 幅度;
    int8_t   方向;

    /* 防御：编号非法直接忽略，避免数组越界 */
    if ((uint32_t)哪台 > (uint32_t)电机_右) {
        return;
    }

    /* 限幅到 -100 ~ +100 */
    if (占空比 > 电机_占空比上限) {
        占空比 = 电机_占空比上限;
    } else if (占空比 < -电机_占空比上限) {
        占空比 = -电机_占空比上限;
    }

    /* 占空比 0：默认按"停止"处理（EN 拉低，电机断电滑行）。
       若把 电机_零即停止 设为 0，则 0 会被当成"最低可转动速度"，
       走下面的死区映射分支，硬件占空比给到 65%。 */
    if ((占空比 == 0) && (电机_零即停止 != 0))
    {
        电机_写方向(哪台, 0);
        __HAL_TIM_SET_COMPARE(&htim3, 电机_PWM通道表[哪台], 0U);
        return;
    }

    方向 = (占空比 >= 0) ? 1 : -1;
    幅度 = (占空比 >= 0) ? (uint16_t)占空比 : (uint16_t)(-占空比);

    /* 死区补偿：软件 1~100 线性映射到硬件 65~100
         硬件 = 最小转动占空比 + 幅度 * (100 - 最小转动占空比) / 100
       这样你只管用 0~100 表达"想要多快"，下面这段负责把死区让出来 */
    硬件占空比 = (uint32_t)电机_最小转动占空比
               + ((uint32_t)幅度 * (100U - (uint32_t)电机_最小转动占空比)) / 100U;

    比较值 = (硬件占空比 * 电机_PWM周期计数) / 100U;

    电机_写方向(哪台, 方向);
    __HAL_TIM_SET_COMPARE(&htim3, 电机_PWM通道表[哪台], 比较值);
}

void 电机_设置双轮(int16_t 左占空比, int16_t 右占空比)
{
    电机_转速(电机_左, 左占空比);
    电机_转速(电机_右, 右占空比);
}

void 电机_停止(电机_编号 哪台)
{
    电机_转速(哪台, 0);
}

void 电机_刹车(电机_编号 哪台)
{
    if ((uint32_t)哪台 > (uint32_t)电机_右) {
        return;
    }

    /* 两个方向脚同时为高 -> 桥的两个下管/上管同时导通 -> 绕组短路制动 */
    HAL_GPIO_WritePin(电机_方向端口表[哪台][0], 电机_方向引脚表[哪台][0], GPIO_PIN_SET);
    HAL_GPIO_WritePin(电机_方向端口表[哪台][1], 电机_方向引脚表[哪台][1], GPIO_PIN_SET);

    /* EN 给满，制动才有效 */
    __HAL_TIM_SET_COMPARE(&htim3, 电机_PWM通道表[哪台], 电机_PWM周期计数);
}

void 电机_全部停止(void)
{
    电机_转速(电机_左, 0);
    电机_转速(电机_右, 0);
}
