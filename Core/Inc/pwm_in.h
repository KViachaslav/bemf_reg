/**
  ******************************************************************************
  * @file    pwm_in.h
  * @brief   Приём управляющего сигнала от grblHAL (прошивка BEMF_CTRL_GRBL=1):
  *          скважность ШИМ на PB6 (TIM4_CH1, capture) -> уставка оборотов,
  *          уровень на PB8 (enable, pull-down) -> разрешение вращения.
  *
  *          Разрешён только в GRBL-сборке: в UART-сборке модуль не собирается
  *          (см. ctrl_mode.h), поэтому прошивка для управления с ПК остаётся
  *          без изменений.
  ******************************************************************************/

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __PWM_IN_H
#define __PWM_IN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "ctrl_mode.h"

#if BEMF_CTRL_GRBL

/* Exported constants --------------------------------------------------------*/

/* --- Вход ШИМ: PB6 = TIM4_CH1 (вход, pull-down; обрыв читается как 0 %) --- */
#define PWM_IN_TIM_INSTANCE     TIM4
#define PWM_IN_TIM_CHANNEL      TIM_CHANNEL_1
#define PWM_IN_TIM_GPIO_PORT    GPIOB
#define PWM_IN_TIM_GPIO_PIN     GPIO_PIN_6
#define PWM_IN_TIM_IRQn         TIM4_IRQn
#define PWM_IN_TIM_IRQ_PREEMPT  3U          /* ниже TIM2 (ПИ) и TIM3 (замер)  */

/* TIM4: 64 МГц / 16 = 4 МГц, тик 250 нс.
   Период 16 бит: минимальная частота входа ~61 Гц, при 1 кГц разрешение
   по скважности 0.025 % (1.5 об/мин при 6000).                           */
#define PWM_IN_TIM_PRESCALER    15U         /* 250 нс на тик                  */
#define PWM_IN_TIM_PERIOD       0xFFFFU     /* максимальный период счёта      */
#define PWM_IN_IC_FILTER        0xFU        /* фильтр входа: 8 замеров по 4 МГц */
#define PWM_IN_TIM_CLOCK_HZ     4000000U    /* 64 МГц / (15 + 1)              */

/* Допустимый период входного сигнала, тиков по 250 нс:
   16 тиков (~250 кГц) ... 65500 тиков (~61 Гц).                        */
#define PWM_IN_PERIOD_MIN_TICKS 16U
#define PWM_IN_PERIOD_MAX_TICKS 65500U

#define PWM_IN_DUTY_FULL_X100   10000U      /* 100.00 %                       */
#define PWM_IN_DUTY_MIN_X100    100U        /* 1.00 % — ниже считаем «стоп»   */

/* Тишина на линии ШИМ дольше этого времени означает «управление остановлено»
   (grbl выключил шпиндель: скважность 0 не даёт фронтов) либо обрыв провода.
   В обоих случаях привод останавливается, а не продолжает вращаться на
   последней уставке.                                                     */
#define PWM_IN_SILENCE_MS       200U
#define PWM_IN_POLL_MS          50U         /* период пересчёта уставки       */

#define PWM_IN_SETPOINT_DEADBAND_RPM 10.0f  /* шаг обновления уставки         */

/* RPM при скважности 100 % (задаётся командой P<rpm>, калибровка под УЧ)   */
#define PWM_IN_RPM_AT_100_DEFAULT   6000.0f
#define PWM_IN_RPM_AT_100_MIN       100.0f
#define PWM_IN_RPM_AT_100_MAX       6000.0f /* = PID_SETPOINT_MAX_RPM         */

/* --- Вход разрешения: PB8 (вход, pull-down: обрыв = «стоп») -------------- */
#define PWM_IN_EN_GPIO_PORT     GPIOB
#define PWM_IN_EN_GPIO_PIN      GPIO_PIN_8

/* Exported functions prototypes ---------------------------------------------*/
void     pwm_in_init(void);             /* GPIO + TIM4 (capture) + NVIC         */
void     pwm_in_poll(void);             /* Вызывается из главного цикла         */

uint8_t  pwm_in_signal_valid(void);     /* Есть свежие захваты ШИМ              */
uint32_t pwm_in_duty_x100(void);        /* Скважность входа, 0..10000           */
uint32_t pwm_in_freq_hz(void);          /* Частота входного ШИМ, Гц             */
uint32_t pwm_in_signal_lost_ms(void);   /* Время с последнего захвата, мс       */
float    pwm_in_setpoint_rpm(void);     /* Уставка из скважности, об/мин        */

uint8_t  pwm_in_enable_level(void);     /* Уровень PB8: 1 — вращать             */
uint8_t  pwm_in_is_uart_override(void); /* 1 — уставка/пуск от UART (M0)        */
void     pwm_in_set_uart_override(uint8_t on);  /* Команды M0/M1 главного цикла */
void     pwm_in_operator_stop(void);    /* X: стоп + блокировка до фронта PB8   */

float    pwm_in_get_rpm_at_100(void);
void     pwm_in_set_rpm_at_100(float rpm);      /* Команда P<rpm>               */

#endif /* BEMF_CTRL_GRBL */

#ifdef __cplusplus
}
#endif

#endif /* __PWM_IN_H */
