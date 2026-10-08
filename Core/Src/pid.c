/**
  ******************************************************************************
  * @file    pid.c
  * @brief   ПИ-регулятор: u(k) = Kp * e(k) + Ki * sum(e) * dt
  *          Anti-windup: ограничение интегратора + вычитание избытка при
  *          насыщении выхода. Вызывается из прерывания TIM2 (1 кГц).
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "pid.h"
#include "motor_pwm.h"
#include "bemf.h"

/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef h_pid_tim;            /* TIM2: 1 мс                          */

/* float в прерывании допустим только для ПИ-регулятора (оговорено в ТЗ)      */
static float    pid_integral = 0.0f;
static float    pid_kp = PID_KP_DEFAULT;
static float    pid_ki = PID_KI_DEFAULT;
static volatile float    pid_setpoint = 0.0f;
static volatile uint16_t pid_output = MOTOR_PWM_DUTY_MIN;
static volatile uint8_t  pid_enabled = 1U;

/* Exported functions --------------------------------------------------------*/

/**
  * @brief  Инициализация TIM2: прерывание каждую 1 мс.
  */
void pid_init(void)
{
  __HAL_RCC_TIM2_CLK_ENABLE();
  h_pid_tim.Instance = TIM2;
  h_pid_tim.Init.Prescaler = PID_TIM_PRESCALER;
  h_pid_tim.Init.CounterMode = TIM_COUNTERMODE_UP;
  h_pid_tim.Init.Period = PID_TIM_PERIOD;
  h_pid_tim.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  h_pid_tim.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&h_pid_tim) != HAL_OK)
  {
    Error_Handler();
  }
  /* Приоритет ниже, чем у BEMF-паузы (TIM3): замер первичен */
  HAL_NVIC_SetPriority(TIM2_IRQn, 2U, 0U);
  HAL_NVIC_EnableIRQ(TIM2_IRQn);
  pid_reset();
  if (HAL_TIM_Base_Start_IT(&h_pid_tim) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief  Шаг регулятора (1 мс). Вызывается из TIM2_IRQHandler.
  */
void pid_isr_1ms(void)
{
  float error;
  float output;
  float output_raw;
  float integral_step;
  float setpoint;
  uint8_t saturated = 0U;

  if ((motor_is_running() == 0U) || (pid_enabled == 0U))
  {
    return;                             /* Открытый цикл или мотор остановлен  */
  }

  setpoint = pid_setpoint;              /* Копия: доступ к уставке атомарен    */
  error = setpoint - bemf_get_rpm();

  /* Deadband: при малой ошибке выход не меняем (защита от дрожания) */
  if ((error < PID_DEADBAND_RPM) && (error > -PID_DEADBAND_RPM))
  {
    motor_set_duty(pid_output);
    return;
  }

  integral_step = error * PID_DT_SECONDS;
  pid_integral += integral_step;
  if (pid_integral > PID_INTEGRAL_MAX)
  {
    pid_integral = PID_INTEGRAL_MAX;
  }
  else if (pid_integral < -PID_INTEGRAL_MAX)
  {
    pid_integral = -PID_INTEGRAL_MAX;
  }
  else
  {
    /* Интегратор в допустимых пределах */
  }

  output_raw = (pid_kp * error) + (pid_ki * pid_integral);
  output = output_raw;

  /* Anti-windup: выход насыщен — из интегратора вычитаем ИЗБЫТОК, делённый
     на Ki (получается значение, при котором выход ровно на границе).
     Отменять целиком последний шаг нельзя: у нижней границы (5 % ШИМ) это
     обнуляет накопление, и ПИ залипает на минимальной скважности (стенд:
     S300 держал DUTY=5.0 % и ERR=293 без изменений).                     */
  if (output > (float)MOTOR_PWM_DUTY_MAX)
  {
    output = (float)MOTOR_PWM_DUTY_MAX;
    saturated = 1U;
  }
  else if (output < (float)MOTOR_PWM_DUTY_MIN)
  {
    output = (float)MOTOR_PWM_DUTY_MIN;
    saturated = 1U;
  }
  else
  {
    /* Выход в допустимом диапазоне */
  }

  if ((saturated != 0U) && (pid_ki > 0.0f))
  {
    pid_integral -= ((output_raw - output) / pid_ki);
  }

  pid_output = (uint16_t)output;
  motor_set_duty(pid_output);
}

/**
  * @brief  Сброс интегратора и выходного значения (старт, fault, смена уставки).
  */
void pid_reset(void)
{
  pid_integral = 0.0f;
  pid_output = MOTOR_PWM_DUTY_MIN;
}

/**
  * @brief  Разрешить/запретить работу регулятора (0 — открытый цикл).
  */
void pid_set_enabled(uint8_t enable)
{
  pid_enabled = (enable != 0U) ? 1U : 0U;
}

/**
  * @brief  Состояние регулятора.
  */
uint8_t pid_is_enabled(void)
{
  return pid_enabled;
}

/**
  * @brief  Задать уставку оборотов (критическая секция — ТЗ п. 6).
  */
void pid_set_setpoint(float rpm)
{
  if (rpm < 0.0f)
  {
    rpm = 0.0f;
  }
  else if (rpm > PID_SETPOINT_MAX_RPM)
  {
    rpm = PID_SETPOINT_MAX_RPM;
  }
  else
  {
    /* Значение в допустимых пределах */
  }

  __disable_irq();
  pid_setpoint = rpm;
  __enable_irq();
}

/**
  * @brief  Текущая уставка оборотов.
  */
float pid_get_setpoint(void)
{
  return pid_setpoint;
}

/**
  * @brief  Задать Kp, Ki (критическая секция — ТЗ п. 6).
  */
void pid_set_gains(float kp, float ki)
{
  if ((kp < 0.0f) || (ki < 0.0f))
  {
    return;                             /* Отрицательные коэффициенты недопустимы */
  }

  __disable_irq();
  pid_kp = kp;
  pid_ki = ki;
  __enable_irq();
}

/**
  * @brief  Прочитать Kp, Ki.
  */
void pid_get_gains(float *kp, float *ki)
{
  if ((kp != NULL) && (ki != NULL))
  {
    *kp = pid_kp;
    *ki = pid_ki;
  }
}

/**
  * @brief  Последнее значение, поданное на ШИМ.
  */
uint16_t pid_get_output(void)
{
  return pid_output;
}
