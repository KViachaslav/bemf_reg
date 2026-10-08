/**
  ******************************************************************************
  * @file    motor_pwm.c
  * @brief   Управление скважностью ШИМ TIM1_CH1 и организация измерительных
  *          пауз для замера BEMF.
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "motor_pwm.h"

/* Private variables ---------------------------------------------------------*/
extern TIM_HandleTypeDef htim1;         /* Сконфигурирован CubeMX (MX_TIM1_Init) */

static uint16_t motor_duty = 0U;        /* Рабочая скважность (CCR1)            */
static uint8_t  motor_running = 0U;     /* 1 — мотор запущен командой R         */

/* Private function prototypes -----------------------------------------------*/
static void motor_apply_duty(uint16_t compare);

/* Exported functions --------------------------------------------------------*/

/**
  * @brief  Инициализация: канал ШИМ запущен, CCR1 = 0.
  * @note   В режиме PWM Mode 1 при CCR1 = 0 выход PA8 постоянно прижат к нулю —
  *         это и есть «закрытый ключ». Отключение через MOE = 0 не используем:
  *         при MOE = 0 выход переходит в Hi-Z и затвор IRLZ44N оказывается
  *         «в воздухе», что недопустимо без внешнего резистора сток-исток.
  */
void motor_pwm_init(void)
{
  motor_duty = 0U;
  motor_running = 0U;
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0U);
  (void)HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
}

/**
  * @brief  Пуск мотора: минимальная скважность, дальше — ПИ-регулятор.
  */
void motor_start(void)
{
  motor_running = 1U;
  motor_apply_duty(MOTOR_PWM_DUTY_MIN);
}

/**
  * @brief  Полная остановка мотора.
  */
void motor_stop(void)
{
  motor_running = 0U;
  motor_apply_duty(0U);
}

/**
  * @brief  Признак работы мотора (используется ПИ и защитой).
  * @retval 1 — мотор запущен, 0 — остановлен
  */
uint8_t motor_is_running(void)
{
  return motor_running;
}

/**
  * @brief  Задать скважность (значение CCR1) с ограничением диапазона.
  * @param  compare: желаемое значение CCR1
  */
void motor_set_duty(uint16_t compare)
{
  if (compare < MOTOR_PWM_DUTY_MIN)
  {
    compare = MOTOR_PWM_DUTY_MIN;
  }
  else if (compare > MOTOR_PWM_DUTY_MAX)
  {
    compare = MOTOR_PWM_DUTY_MAX;
  }
  else
  {
    /* Значение уже в допустимом диапазоне */
  }
  motor_apply_duty(compare);
}

/**
  * @brief  Текущая рабочая скважность (CCR1).
  */
uint16_t motor_get_duty(void)
{
  return motor_duty;
}

/**
  * @brief  Скважность в процентах (для телеметрии).
  */
float motor_get_duty_percent(void)
{
  return ((float)motor_duty * 100.0f) / ((float)MOTOR_PWM_PERIOD + 1.0f);
}

/**
  * @brief  Измерительная пауза: ключ закрыт, мотор вращается по инерции.
  *         Рабочая скважность сохраняется в motor_duty.
  */
void motor_pwm_pause(void)
{
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0U);
}

/**
  * @brief  Возврат ШИМ к рабочей скважности после измерительной паузы.
  */
void motor_pwm_resume(void)
{
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, (uint32_t)motor_duty);
}

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Записать значение CCR1 и запомнить его как рабочую скважность.
  */
static void motor_apply_duty(uint16_t compare)
{
  motor_duty = compare;
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, (uint32_t)compare);
}
