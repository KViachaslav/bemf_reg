/**
  ******************************************************************************
  * @file    fault.h
  * @brief   Защита: stall (заклинивание), недостоверный BEMF, потеря управления
  *          (UART — сборка UART; пропадание ШИМ grbl при активном PB8 — сборка
  *          GRBL), просадка питания (SUPPLY_LOW), обрыв тракта (SENSOR_FAULT).
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __FAULT_H
#define __FAULT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "ctrl_mode.h"

/* Exported constants --------------------------------------------------------*/
#define FAULT_NONE              0U      /* Аварии нет                            */
#define FAULT_STALL             1U      /* Заклинивание мотора                   */
#define FAULT_BEMF_INVALID      2U      /* Недостоверный BEMF                    */
#define FAULT_UART_LOST         3U      /* Потеря связи с ПК                     */
#define FAULT_SUPPLY_LOW        4U      /* Просадка/отсутствие питания мотора    */
#define FAULT_SENSOR_FAULT      5U      /* Обрыв измерительного тракта (ADC)     */
#define FAULT_PWM_LOST          6U      /* Нет ШИМ от grbl при PB8 = 1 (GRBL)    */

#define FAULT_STALL_SETPOINT_RPM  500.0f /* Уставка, выше которой следим за stall */
#define FAULT_STALL_MEASURED_RPM  100.0f /* Порог оборотов для stall              */
#define FAULT_STALL_TIME_MS       500U   /* Время подтверждения stall, мс         */
#define FAULT_BEMF_INVALID_LIMIT  3U     /* Недостоверных замеров подряд           */
#define FAULT_SUPPLY_MIN_VOLTS    6.0f   /* Минимальное питание мотора, В         */
#define FAULT_SENSOR_LIMIT        3U     /* Замеров подряд «оба входа в нуле»     */
#define FAULT_PWM_LOST_MS         500U   /* Подтверждение потери ШИМ grbl, мс     */
#define FAULT_UART_TIMEOUT_MS     5000U  /* Таймаут команд UART, мс               */
#define FAULT_LED_HALF_PERIOD_MS  100U   /* Мигание LED 5 Гц (полупериод 100 мс)  */

/* Exported functions prototypes ---------------------------------------------*/
void     fault_init(void);
void     fault_isr_1ms(void);           /* Вызывается из TIM2 IRQ               */
uint8_t  fault_is_active(void);
uint8_t  fault_get_code(void);
void     fault_clear(void);             /* Сброс — только команда X             */

#ifdef __cplusplus
}
#endif

#endif /* __FAULT_H */
