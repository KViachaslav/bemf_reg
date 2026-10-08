/**
  ******************************************************************************
  * @file    motor_pwm.h
  * @brief   Управление мотором через TIM1 CH1 (PA8) + организация измерительных
  *          пауз ШИМ для замера BEMF.
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MOTOR_PWM_H
#define __MOTOR_PWM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Exported constants --------------------------------------------------------*/
/* TIM1: 64 МГц / (Prescaler+1) / (Period+1) = 64e6 / 3200 = 20 кГц           */
#define MOTOR_PWM_PERIOD        3199U   /* ARR (Counter Period), CubeMX        */
#define MOTOR_PWM_PRESCALER     0U      /* PSC                                  */

/* Ограничения скважности (ТЗ п. «Компромиссы»):                              */
#define MOTOR_PWM_DUTY_MIN      160U    /* 5 %  — ниже ШИМ не питает мотор      */
#define MOTOR_PWM_DUTY_MAX      3040U   /* 95 % — при 100 % нет пауз для BEMF   */

/* Exported functions prototypes ---------------------------------------------*/
void     motor_pwm_init(void);          /* Запуск ШИМ-канала с нулевой скважностью */
void     motor_start(void);             /* Пуск: скважность = MOTOR_PWM_DUTY_MIN   */
void     motor_stop(void);              /* Стоп: скважность = 0, ключ закрыт       */
uint8_t  motor_is_running(void);
void     motor_set_duty(uint16_t compare);
uint16_t motor_get_duty(void);
float    motor_get_duty_percent(void);
void     motor_pwm_pause(void);         /* Измерительная пауза (ключ закрыт)       */
void     motor_pwm_resume(void);        /* Возврат к рабочей скважности            */

#ifdef __cplusplus
}
#endif

#endif /* __MOTOR_PWM_H */
