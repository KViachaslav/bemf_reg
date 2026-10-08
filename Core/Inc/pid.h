/**
  ******************************************************************************
  * @file    pid.h
  * @brief   ПИ-регулятор оборотов с anti-windup. Вызов — 1 кГц (TIM2).
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __PID_H
#define __PID_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Exported constants --------------------------------------------------------*/
/* Коэффициенты подобраны на стенде (мотор 775, K_motor = 330, питание 12 В):
   при `K1.5,0.6` и `S1500` выход на уставку плавный, без перерегулирования
   (проверено прогонами motor_t*.txt); `Kp=2.5` даёт раскачку оборотов.     */
#define PID_KP_DEFAULT          1.5f    /* Пропорциональный коэффициент          */
#define PID_KI_DEFAULT          0.6f    /* Интегральный коэффициент              */
#define PID_DT_SECONDS          0.001f  /* Период вызова регулятора (1 кГц)      */
/* Ограничение интегратора должно покрывать диапазон скважности:
   Ki * I_MAX >= MOTOR_PWM_DUTY_MAX (при Ki = 0.25 это 12160). Значение 2000
   на стенде оказалось мало: вклад I-члена упирался в 500 отсчётов (15.6 %
   ШИМ), и уставки выше ~600 RPM не достигались (см. README §16).          */
#define PID_INTEGRAL_MAX        12000.0f /* Ограничение интегратора (anti-windup) */
#define PID_DEADBAND_RPM        20.0f   /* Мёртвая зона по ошибке, RPM           */
#define PID_SETPOINT_MAX_RPM    6000.0f /* Ограничение уставки                   */

#define PID_TIM_PRESCALER       63U     /* 64 МГц / 64 = 1 МГц                   */
#define PID_TIM_PERIOD          999U    /* 1 МГц / 1000 = 1 кГц                  */

/* Exported functions prototypes ---------------------------------------------*/
void     pid_init(void);                /* TIM2 + NVIC                          */
void     pid_isr_1ms(void);             /* Вызывается из TIM2 IRQ               */
void     pid_reset(void);               /* Сброс интегратора и выхода           */
void     pid_set_enabled(uint8_t enable); /* 0 — открытый цикл (калибровка)     */
uint8_t  pid_is_enabled(void);
void     pid_set_setpoint(float rpm);
float    pid_get_setpoint(void);
void     pid_set_gains(float kp, float ki);
void     pid_get_gains(float *kp, float *ki);
uint16_t pid_get_output(void);

#ifdef __cplusplus
}
#endif

#endif /* __PID_H */
