/**
  ******************************************************************************
  * @file    pwm_in.c
  * @brief   Приём управляющего сигнала от grblHAL (только сборка GRBL):
  *          PB6 (TIM4_CH1, input capture) — скважность ШИМ = уставка оборотов,
  *          PB8 — разрешение вращения (1 = вращать, 0 = стоп).
  *
  *          Алгоритм замера: один канал, полярность переключается в прерывании
  *          «фронт -> спад -> фронт». При каждом захвате аппаратно фиксируется
  *          TIM4->CCR1, поэтому задержка входа в прерывание точность не портит:
  *              высокий уровень = t(спад) - t(фронт)
  *              период          = t(фронт_N) - t(фронт_N-1)
  *              скважность[%*100] = высокий * 10000 / период
  *          Если спад не пришёл (линия зажата в «1») — скважность 100 %.
  *
  *          Безопасность:
  *            • PB8 = 0 (в т.ч. обрыв провода — pull-down) -> немедленный стоп;
  *            • тишина на линии ШИМ > PWM_IN_SILENCE_MS -> стоп (grbl выключил
  *              шпиндель командой M5/S0, у которого скважность 0 не даёт
  *              фронтов, либо обрыв управляющего провода);
  *            • переход PB8 0->1 (новый M3) снимает блокировку X и сбрасывает
  *              аварию, чтобы станок без консоли восстанавливался сам.
  ******************************************************************************/

/* Includes ------------------------------------------------------------------*/
#include "pwm_in.h"

#if BEMF_CTRL_GRBL

#include "motor_pwm.h"
#include "pid.h"
#include "fault.h"

/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef h_pwm_in_tim;          /* TIM4: захват ШИМ (extern в it.c)   */

/* --- Переменные прерывания захвата (TIM4) --- */
static volatile uint32_t pwm_period_ticks  = 0U;  /* Период входа, тиков       */
static volatile uint32_t pwm_high_ticks    = 0U;  /* Высокий уровень, тиков    */
static volatile uint32_t pwm_duty_x100     = 0U;  /* Скважность, 0..10000      */
static volatile uint32_t pwm_last_capture_ms = 0U;/* Тик последнего захвата    */
static volatile uint32_t pwm_capture_count = 0U;  /* Всего захватов            */
static volatile uint8_t  pwm_expect_rise   = 1U;  /* 1 — ждём фронт, 0 — спад  */

static uint16_t pwm_rise_time = 0U;               /* Время последнего фронта   */
static uint8_t  pwm_have_rise = 0U;               /* Фронт уже был             */
static uint8_t  pwm_cycle_fall = 0U;              /* В цикле был спад          */

/* --- Переменные главного цикла --- */
static float    pwm_rpm_at_100   = PWM_IN_RPM_AT_100_DEFAULT;
static float    pwm_setpoint_rpm = 0.0f;          /* Переданная в ПИ уставка   */
static uint8_t  pwm_uart_override = 0U;           /* 1 — управление от UART    */
static uint8_t  pwm_operator_stop = 0U;           /* 1 — блокировка после X    */
static uint8_t  pwm_en_prev  = 0U;                /* Предыдущий уровень PB8    */
static uint32_t pwm_poll_tick = 0U;

/* Private function prototypes -----------------------------------------------*/
static void  pwm_in_gpio_init(void);
static void  pwm_in_set_polarity(uint8_t expect_rise);
static float pwm_in_duty_to_rpm(uint32_t duty_x100);

/* Exported functions --------------------------------------------------------*/

/**
  * @brief  Настройка входов PB6/PB8 и запуск захвата TIM4 по прерыванию.
  */
void pwm_in_init(void)
{
  TIM_IC_InitTypeDef ic = {0};

  /* PB6 — вход захвата ШИМ, PB8 — вход разрешения. Подтяжка вниз: обрыв
     провода читается как «стоп»/«0 %», а не как случайный уровень.       */
  pwm_in_gpio_init();

  /* TIM4 в режиме Input Capture: хватает одного канала CH1, полярность
     переключается в прерывании (фронт -> спад -> фронт).                  */
  __HAL_RCC_TIM4_CLK_ENABLE();
  h_pwm_in_tim.Instance               = PWM_IN_TIM_INSTANCE;
  h_pwm_in_tim.Init.Prescaler         = PWM_IN_TIM_PRESCALER;
  h_pwm_in_tim.Init.CounterMode       = TIM_COUNTERMODE_UP;
  h_pwm_in_tim.Init.Period            = PWM_IN_TIM_PERIOD;
  h_pwm_in_tim.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
  h_pwm_in_tim.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_IC_Init(&h_pwm_in_tim) != HAL_OK)
  {
    Error_Handler();
  }

  ic.ICPolarity  = TIM_INPUTCHANNELPOLARITY_RISING;
  ic.ICSelection = TIM_ICSELECTION_DIRECTTI;
  ic.ICPrescaler = TIM_ICPSC_DIV1;
  ic.ICFilter    = PWM_IN_IC_FILTER;
  if (HAL_TIM_IC_ConfigChannel(&h_pwm_in_tim, &ic, PWM_IN_TIM_CHANNEL) != HAL_OK)
  {
    Error_Handler();
  }

  /* Приоритет ниже, чем у TIM2 (ПИ + защиты) и TIM3 (замеры BEMF). */
  HAL_NVIC_SetPriority(PWM_IN_TIM_IRQn, PWM_IN_TIM_IRQ_PREEMPT, 0U);
  HAL_NVIC_EnableIRQ(PWM_IN_TIM_IRQn);

  if (HAL_TIM_IC_Start_IT(&h_pwm_in_tim, PWM_IN_TIM_CHANNEL) != HAL_OK)
  {
    Error_Handler();
  }

  /* До первых фронтов линия считается «молчащей» — привод не запускается. */
  pwm_expect_rise     = 1U;
  pwm_have_rise       = 0U;
  pwm_cycle_fall      = 0U;
  pwm_rise_time       = 0U;
  pwm_period_ticks    = 0U;
  pwm_high_ticks      = 0U;
  pwm_duty_x100       = 0U;
  pwm_capture_count   = 0U;
  pwm_last_capture_ms = HAL_GetTick() - (uint32_t)PWM_IN_SILENCE_MS;
  pwm_setpoint_rpm    = 0.0f;
  pwm_en_prev         = pwm_in_enable_level();
  pwm_poll_tick       = HAL_GetTick();
}

/**
  * @brief  Колбэк HAL (прерывание TIM4): обработка аппаратного захвата CH1.
  * @note   Вызывается из HAL_TIM_IRQHandler, флаг захвата уже сброшен.
  */
void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
{
  uint16_t time;
  uint32_t period;
  uint32_t high;
  uint32_t duty;

  if ((htim->Instance != PWM_IN_TIM_INSTANCE) ||
      (htim->Channel != HAL_TIM_ACTIVE_CHANNEL_1))
  {
    return;
  }

  time = (uint16_t)PWM_IN_TIM_INSTANCE->CCR1;
  pwm_last_capture_ms = HAL_GetTick();
  pwm_capture_count++;

  if (pwm_expect_rise != 0U)
  {
    /* Захвачен фронт: закрываем предыдущий период. */
    if (pwm_have_rise != 0U)
    {
      period = (uint32_t)(uint16_t)(time - pwm_rise_time);
      pwm_period_ticks = period;
      if ((pwm_cycle_fall == 0U) && (period != 0U))
      {
        /* Спада в предыдущем цикле не было — линия зажата в «1»: 100 %. */
        pwm_high_ticks = period;
        pwm_duty_x100  = PWM_IN_DUTY_FULL_X100;
      }
    }
    pwm_rise_time  = time;
    pwm_have_rise  = 1U;
    pwm_cycle_fall = 0U;
    pwm_in_set_polarity(0U);            /* Далее ловим спад                    */
  }
  else
  {
    /* Захвачен спад: длительность высокого уровня. */
    high = (uint32_t)(uint16_t)(time - pwm_rise_time);
    pwm_high_ticks = high;
    pwm_cycle_fall = 1U;

    period = pwm_period_ticks;
    if (period != 0U)
    {
      duty = (high * PWM_IN_DUTY_FULL_X100) / period;
      if (duty > PWM_IN_DUTY_FULL_X100)
      {
        duty = PWM_IN_DUTY_FULL_X100;
      }
      pwm_duty_x100 = duty;
    }
    pwm_in_set_polarity(1U);            /* Далее ловим фронт                   */
  }
}

/**
  * @brief  Главный цикл: уровни PB8/ШИМ -> пуск/стоп/уставка (раз в 50 мс).
  */
void pwm_in_poll(void)
{
  uint8_t  enable;
  uint32_t duty;
  float    rpm;

  if ((HAL_GetTick() - pwm_poll_tick) < (uint32_t)PWM_IN_POLL_MS)
  {
    return;
  }
  pwm_poll_tick = HAL_GetTick();

  /* 1. Разрешение вращения (PB8). Ноль — стоп в любом режиме. */
  enable = pwm_in_enable_level();
  if ((pwm_en_prev == 0U) && (enable != 0U))
  {
    /* Новый цикл разрешения (M5 -> M3): снимаем блокировку X и сбрасываем
       аварию, чтобы станок без консоли восстанавливался сам.              */
    pwm_operator_stop = 0U;
    fault_clear();
  }
  pwm_en_prev = enable;

  /* Управление от grbl восстановилось — снимаем аварию «нет сигнала ШИМ». */
  if ((enable != 0U) && (fault_get_code() == FAULT_PWM_LOST) &&
      (pwm_in_signal_valid() != 0U))
  {
    fault_clear();
  }

  if (enable == 0U)
  {
    if (motor_is_running() != 0U)
    {
      motor_stop();
      pid_reset();
    }
    pwm_setpoint_rpm = 0.0f;
    return;
  }

  /* 2. Авария или блокировка после X — привод не запускаем. */
  if ((fault_is_active() != 0U) || (pwm_operator_stop != 0U))
  {
    if (motor_is_running() != 0U)
    {
      motor_stop();
      pid_reset();
    }
    return;
  }

  /* 3. Команда M0: уставкой и пуском распоряжается консоль UART. */
  if (pwm_uart_override != 0U)
  {
    return;
  }

  /* 4. Скважность входа: «тишина» или < 1 % — это стоп (M5/S0 или обрыв). */
  duty = pwm_in_duty_x100();
  if ((pwm_in_signal_valid() == 0U) || (duty < PWM_IN_DUTY_MIN_X100))
  {
    if (motor_is_running() != 0U)
    {
      motor_stop();
      pid_reset();
    }
    pwm_setpoint_rpm = 0.0f;
    return;
  }

  /* 5. Скважность -> уставка оборотов, слежение за S-командой grbl. */
  rpm = pwm_in_duty_to_rpm(duty);
  if (rpm > PID_SETPOINT_MAX_RPM)
  {
    rpm = PID_SETPOINT_MAX_RPM;
  }

  if (motor_is_running() == 0U)
  {
    pid_reset();                        /* Пуск: интегратор с нуля             */
    pid_set_setpoint(rpm);
    pwm_setpoint_rpm = rpm;
    motor_start();
  }
  else if (((rpm - pwm_setpoint_rpm) > PWM_IN_SETPOINT_DEADBAND_RPM) ||
           ((pwm_setpoint_rpm - rpm) > PWM_IN_SETPOINT_DEADBAND_RPM))
  {
    pid_set_setpoint(rpm);              /* Плавное слежение: интегратор жив    */
    pwm_setpoint_rpm = rpm;
  }
  else
  {
    /* Изменение меньше мёртвой зоны — уставку не трогаем. */
  }
}

/**
  * @brief  Есть ли на входе свежий достоверный сигнал ШИМ.
  */
uint8_t pwm_in_signal_valid(void)
{
  uint32_t period = pwm_period_ticks;

  if (pwm_in_signal_lost_ms() >= (uint32_t)PWM_IN_SILENCE_MS)
  {
    return 0U;                          /* Фронтов нет — управление пропало    */
  }
  if (pwm_capture_count < 3U)
  {
    return 0U;                          /* Не хватает данных для периода       */
  }
  if ((period < PWM_IN_PERIOD_MIN_TICKS) || (period > PWM_IN_PERIOD_MAX_TICKS))
  {
    return 0U;                          /* Частота вне допустимого диапазона   */
  }
  return 1U;
}

/**
  * @brief  Скважность входного ШИМ в сотых долях процента (0..10000).
  */
uint32_t pwm_in_duty_x100(void)
{
  return (pwm_in_signal_valid() != 0U) ? pwm_duty_x100 : 0U;
}

/**
  * @brief  Частота входного ШИМ, Гц (0 — сигнала нет).
  */
uint32_t pwm_in_freq_hz(void)
{
  uint32_t period = pwm_period_ticks;

  if ((period == 0U) || (pwm_in_signal_valid() == 0U))
  {
    return 0U;
  }
  return (PWM_IN_TIM_CLOCK_HZ / period);
}

/**
  * @brief  Время с последнего захвата ШИМ, мс.
  */
uint32_t pwm_in_signal_lost_ms(void)
{
  return HAL_GetTick() - pwm_last_capture_ms;
}

/**
  * @brief  Уставка, переданная в ПИ из скважности входного ШИМ, об/мин.
  */
float pwm_in_setpoint_rpm(void)
{
  return pwm_setpoint_rpm;
}

/**
  * @brief  Уровень разрешения вращения (PB8): 1 — вращать, 0 — стоп.
  */
uint8_t pwm_in_enable_level(void)
{
  return (HAL_GPIO_ReadPin(PWM_IN_EN_GPIO_PORT, PWM_IN_EN_GPIO_PIN) == GPIO_PIN_SET)
             ? 1U : 0U;
}

/**
  * @brief  Признак «уставкой управляет UART» (команда M0).
  */
uint8_t pwm_in_is_uart_override(void)
{
  return pwm_uart_override;
}

/**
  * @brief  Переключение источника уставки: 1 — UART (M0), 0 — grbl (M1).
  * @note   При переходе в режим UART привод останавливается: дальше пуск
  *         выполняется командой R, а уставка — командой S.
  */
void pwm_in_set_uart_override(uint8_t on)
{
  pwm_uart_override = (on != 0U) ? 1U : 0U;
  if (pwm_uart_override != 0U)
  {
    motor_stop();
    pid_reset();
    pwm_setpoint_rpm = 0.0f;
  }
}

/**
  * @brief  Команда X: останов + запрет автозапуска от PB8 до его фронта.
  */
void pwm_in_operator_stop(void)
{
  pwm_operator_stop = 1U;
  motor_stop();
  pid_reset();
  pwm_setpoint_rpm = 0.0f;
}

/**
  * @brief  Обороты, соответствующие скважности 100 %, об/мин.
  */
float pwm_in_get_rpm_at_100(void)
{
  return pwm_rpm_at_100;
}

/**
  * @brief  Задать обороты при скважности 100 % (команда P<rpm>).
  */
void pwm_in_set_rpm_at_100(float rpm)
{
  if (rpm < PWM_IN_RPM_AT_100_MIN)
  {
    rpm = PWM_IN_RPM_AT_100_MIN;
  }
  if (rpm > PWM_IN_RPM_AT_100_MAX)
  {
    rpm = PWM_IN_RPM_AT_100_MAX;
  }
  pwm_rpm_at_100 = rpm;
}

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Входы PB6 (захват ШИМ) и PB8 (разрешение) с подтяжкой вниз.
  */
static void pwm_in_gpio_init(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOB_CLK_ENABLE();

  gpio.Pin   = PWM_IN_TIM_GPIO_PIN | PWM_IN_EN_GPIO_PIN;
  gpio.Mode  = GPIO_MODE_INPUT;
  gpio.Pull  = GPIO_PULLDOWN;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(PWM_IN_TIM_GPIO_PORT, &gpio);
}

/**
  * @brief  Переключение полярности захвата (0 — ждём спад, 1 — фронт).
  */
static void pwm_in_set_polarity(uint8_t expect_rise)
{
  if (expect_rise != 0U)
  {
    PWM_IN_TIM_INSTANCE->CCER &= (uint16_t)(~TIM_CCER_CC1P);
  }
  else
  {
    PWM_IN_TIM_INSTANCE->CCER |= (uint16_t)TIM_CCER_CC1P;
  }
  pwm_expect_rise = expect_rise;
}

/**
  * @brief  Пересчёт скважности (0..10000) в уставку оборотов.
  */
static float pwm_in_duty_to_rpm(uint32_t duty_x100)
{
  return (pwm_rpm_at_100 * (float)duty_x100) / (float)PWM_IN_DUTY_FULL_X100;
}

#endif /* BEMF_CTRL_GRBL */


