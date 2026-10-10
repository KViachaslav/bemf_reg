/**
  ******************************************************************************
  * @file    bemf.h
  * @brief   Дифференциальное измерение обратной ЭДС (BEMF) двумя каналами ADC1:
  *          V_bemf = V_supply - V_low. Замер выполняется в паузах ШИМ,
  *          планировщик пауз — TIM3 (1 мс).
  *
  *          ВАЖНО: в коде фигурируют не «номера каналов», а РОЛИ входов — низкая
  *          сторона (V_low) и питание (V_supply). Делители 10к/2к на обоих
  *          входах одинаковые, поэтому перекрёстная разводка платы полностью
  *          компенсируется макросом BEMF_ADC_CH_SWAP (см. ниже).
  *
  *          Базовая плата      (BEMF_ADC_CH_SWAP = 0):
  *              PA0 (ADC1_IN0) — низкая сторона мотора (V_low);
  *              PA1 (ADC1_IN1) — питание мотора       (V_supply).
  *          Плата варианта GRBL (BEMF_ADC_CH_SWAP = 1, README §18.7) —
  *          выводы A0/A1 на ней разведены наоборот:
  *              PA1 (ADC1_IN1) — низкая сторона мотора (V_low);
  *              PA0 (ADC1_IN0) — питание мотора       (V_supply).
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __BEMF_H
#define __BEMF_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "ctrl_mode.h"

/* Exported constants --------------------------------------------------------*/

/* Перекрёстная разводка входов измерения: 1 — поменять каналы A0/A1 местами.
   На плате варианта GRBL выводы A0/A1 разведены наоборот (README §18.7),
   поэтому для сборки GRBL перестановка включена по умолчанию; ключом
   -DBEMF_ADC_CH_SWAP=0|1 её можно выключить/включить принудительно.          */
#ifndef BEMF_ADC_CH_SWAP
#define BEMF_ADC_CH_SWAP        BEMF_CTRL_GRBL
#endif

#if (BEMF_ADC_CH_SWAP != 0) && (BEMF_ADC_CH_SWAP != 1)
#error "BEMF_ADC_CH_SWAP: допустимы только 0 (как на базовой плате) и 1 (A0/A1 местами)"
#endif

/* Каналы сканирования ADC1: ранг 1 — низкая сторона, ранг 2 — питание.
   Номер канала = номер пина платы (IN0 = PA0/A0, IN1 = PA1/A1) и зависит от
   варианта разводки: BEMF_ADC_CH_SWAP.                                       */
#if BEMF_ADC_CH_SWAP
#define BEMF_ADC_CHANNEL_LOW    1U          /* ADC1_IN1 (PA1) — низкая сторона   */
#define BEMF_ADC_CHANNEL_SUPPLY 0U          /* ADC1_IN0 (PA0) — питание мотора   */
#else
#define BEMF_ADC_CHANNEL_LOW    0U          /* ADC1_IN0 (PA0) — низкая сторона   */
#define BEMF_ADC_CHANNEL_SUPPLY 1U          /* ADC1_IN1 (PA1) — питание мотора   */
#endif
#define BEMF_ADC_RANK_LOW       1U          /* Ранг 1 — низкая сторона (буфер[0]) */
#define BEMF_ADC_RANK_SUPPLY    2U          /* Ранг 2 — питание (буфер[1])        */
#define BEMF_ADC_CHANNEL_COUNT  2U          /* Каналов в одном сканировании      */

/* Обслуживание ADC1: DMA1 Channel 1 (Circular)                                */
#define BEMF_DMA_INSTANCE       DMA1_Channel1
#define BEMF_DMA_IRQn           DMA1_Channel1_IRQn
#define BEMF_DMA_IRQ_PREEMPT    0U          /* приоритет выше TIM3 (см. bemf.c)  */

/* Делитель 10к/2к стоит на ОБОИХ входах измерения (A0 и A1) — поэтому
   перестановка каналов BEMF_ADC_CH_SWAP не меняет масштаб и не требует
   других правок в расчёте напряжений                                       */
#define BEMF_R1_OHM             10000.0f    /* верхнее плечо делителя 10к/2к     */
#define BEMF_R2_OHM             2000.0f     /* нижнее плечо делителя             */
#define BEMF_VREF_VOLTS         3.3f        /* опорное напряжение ADC            */
#define BEMF_ADC_FULL_SCALE     4095.0f     /* 12-битный АЦП                     */

#define BEMF_TIM_PRESCALER      63U         /* 64 МГц / 64 = 1 МГц               */
#define BEMF_TIM_PERIOD         999U        /* 1 МГц / 1000 = 1 кГц (1 мс)       */

#define BEMF_MEASURE_PERIOD_MS  20U         /* период процедуры замера, мс       */
#define BEMF_SETTLE_US          1000U       /* пауза стабилизации ЭДС, мкс (1 мс) */
#define BEMF_AVG_DEPTH          8U          /* скользящее среднее по 8 замерам    */

#define BEMF_RAW_INVALID_LO     1U          /* «недостоверный» нижний край АЦП    */
#define BEMF_RAW_INVALID_HI     4094U       /* «недостоверный» верхний край АЦП   */
#define BEMF_VBEMF_MARGIN_V     0.20f       /* допуск на шум при проверке V_bemf  */
#define BEMF_DMA_GUARD_ITER     20000U      /* защита от зависания ожидания DMA   */

/* Калибровка под конкретный мотор: RPM = V_bemf * K_motor, K_motor = об/мин
   на вольт. Для мотора 775 «12-36 В, 4000-12000 об/мин» масштаб взят из
   паспорта (4000/12 = 12000/36 ≈ 333), на стенде подтверждён командой
   `C` + `C<rpm>`: получено K = 330.                                      */
#define BEMF_K_MOTOR_DEFAULT    330.0f      /* RPM = V_bemf * K_motor (калибровка) */
#define BEMF_RPM_SIGN           (+1.0f)     /* Знак направления вращения          */

/* Exported functions prototypes ---------------------------------------------*/
void     bemf_init(void);               /* ADC1 (2 канала + DMA) + TIM3 + DWT   */
void     bemf_isr_tick(void);           /* Вызывается из TIM3 IRQ каждую 1 мс   */
uint16_t bemf_get_raw(void);            /* Последний код АЦП низкой стороны     */
uint16_t bemf_get_raw_average(void);    /* Скользящее среднее кода низкой стороны */
float    bemf_get_volts(void);          /* V_bemf = V_supply - V_low, В         */
float    bemf_get_supply_volts(void);   /* Измеренное питание мотора, В         */
float    bemf_get_rpm(void);            /* Обороты по BEMF                      */
void     bemf_set_k_motor(float k_motor);
float    bemf_get_k_motor(void);
uint8_t  bemf_get_invalid_streak(void); /* счётчик недостоверных замеров подряд */
uint8_t  bemf_get_sensor_streak(void);  /* счётчик замеров «оба входа в нуле»   */
uint8_t  bemf_data_valid(void);         /* Есть хотя бы один достоверный замер  */
void     bemf_reset_invalid_streak(void);

#ifdef __cplusplus
}
#endif

#endif /* __BEMF_H */
