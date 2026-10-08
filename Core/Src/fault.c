/**
  ******************************************************************************
  * @file    fault.c
  * @brief   Защита привода: обрыв измерительного тракта (SENSOR_FAULT),
  *          просадка питания мотора (SUPPLY_LOW), заклинивание (stall),
  *          недостоверный BEMF, потеря связи по UART. Fault гасит ШИМ,
  *          сбрасывает интегратор ПИ и мигает LED PC13 с частотой 5 Гц.
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "fault.h"
#include "motor_pwm.h"
#include "bemf.h"
#include "pid.h"
#include "uart_cmd.h"

/* Private define ------------------------------------------------------------*/
/* Светодиод PC13 на плате Blue Pill подключён к +3.3 В через резистор,
   поэтому он горит при низком уровне на выводе (active low).             */
#define FAULT_LED_ON()   HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET)
#define FAULT_LED_OFF()  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET)

/* Private variables ---------------------------------------------------------*/
static volatile uint8_t fault_code = FAULT_NONE;
static uint16_t stall_ms = 0U;          /* Время подтверждения stall            */
static uint16_t led_ms = 0U;            /* Полупериод мигания LED               */
static uint8_t  led_state = 0U;

/* Private function prototypes -----------------------------------------------*/
static void fault_raise(uint8_t code);

/* Exported functions --------------------------------------------------------*/

/**
  * @brief  Инициализация модуля защиты.
  */
void fault_init(void)
{
  fault_code = FAULT_NONE;
  stall_ms = 0U;
  led_ms = 0U;
  led_state = 0U;
  FAULT_LED_OFF();                      /* LED погашен                         */
}

/**
  * @brief  Тик защиты (1 мс), вызывается из TIM2_IRQHandler.
  * @note   Сравнения float выполняются над уже посчитанными значениями
  *         (как и ПИ-регулятор, это оговорено в ТЗ); тяжёлых вычислений нет.
  */
void fault_isr_1ms(void)
{
  uint8_t no_rotation = 0U;             /* Признак отсутствия вращения          */
  uint8_t high_setpoint = 0U;           /* Уставка выше порога stall            */

  /* --- Индикация: авария — мигание 5 Гц, иначе LED погашен ---------------- */
  led_ms++;
  if (led_ms >= FAULT_LED_HALF_PERIOD_MS)
  {
    led_ms = 0U;
    if (fault_code != FAULT_NONE)
    {
      led_state = (uint8_t)((led_state == 0U) ? 1U : 0U);
      if (led_state != 0U)
      {
        FAULT_LED_ON();
      }
      else
      {
        FAULT_LED_OFF();
      }
    }
    else if (led_state != 0U)
    {
      led_state = 0U;
      FAULT_LED_OFF();
    }
    else
    {
      /* LED уже погашен */
    }
  }

  /* Проверки выполняем только на работающем приводе: остановленному мотору
     заклинивание или потеря связи не угрожают                              */
  if ((fault_code != FAULT_NONE) || (motor_is_running() == 0U))
  {
    stall_ms = 0U;
    return;
  }

  /* --- 1. Обрыв измерительного тракта: оба входа ADC1 в нуле 3 замера -----
     Делители 10к/2к на PA0 и PA1 не могут дать нуль одновременно, пока есть
     питание мотора: это обрыв или неподключённый датчик. Диагноз выдаём
     раньше проверки питания, чтобы обрыв не выглядел как просадка 12 В.   */
  if (bemf_get_sensor_streak() >= FAULT_SENSOR_LIMIT)
  {
    fault_raise(FAULT_SENSOR_FAULT);
    return;
  }

  /* --- 2. Просадка питания: измеренное питание мотора ниже 6 В ------------
     Проверяем только после первого достоверного замера — до него v_supply
     ещё не измерено, и сравнение дало бы ложную аварию при пуске.          */
  if ((bemf_data_valid() != 0U) && (bemf_get_supply_volts() < FAULT_SUPPLY_MIN_VOLTS))
  {
    fault_raise(FAULT_SUPPLY_LOW);
    return;
  }

  /* --- 3. Stall: уставка > 500 RPM, вращение не подтверждается 500 мс ----
     Заклинивший мотор не даёт ЭДС: низкая сторона «подтянута» к питанию,
     V_bemf = V_supply - V_low ≈ 0, обороты в нуле. Дополнительно учитываем
     признак недостоверного замера: если замеры отбрасываются (обрыв делителя,
     неисправный ключ), bemf_rpm «замерзает» на старом значении и без этого
     признака stall никогда не подтвердился бы.                            */
  high_setpoint = (pid_get_setpoint() > FAULT_STALL_SETPOINT_RPM) ? 1U : 0U;
  if (high_setpoint != 0U)
  {
    if ((bemf_get_rpm() < FAULT_STALL_MEASURED_RPM) || (bemf_get_invalid_streak() > 0U))
    {
      no_rotation = 1U;
    }
  }

  if (no_rotation != 0U)
  {
    if (stall_ms < FAULT_STALL_TIME_MS)
    {
      stall_ms++;
    }
    if (stall_ms >= FAULT_STALL_TIME_MS)
    {
      fault_raise(FAULT_STALL);
    }
  }
  else
  {
    stall_ms = 0U;
  }

  /* --- 4. Недостоверный BEMF: 0 или 4095 три замера подряд ----------------
     При высокой уставке такое показание — ожидаемое следствие заклинивания,
     диагноз отдан stall-детектору выше. В остальных случаях упор АЦП в
     крайние коды означает неисправность измерительного тракта — отключаемся
     сразу.                                                                */
  if ((high_setpoint == 0U) && (bemf_get_invalid_streak() >= FAULT_BEMF_INVALID_LIMIT))
  {
    fault_raise(FAULT_BEMF_INVALID);
  }

  /* --- 5. Потеря связи: нет команд ПК дольше 5 с ------------------------- */
  if (uart_cmd_ms_since_last() > (uint32_t)FAULT_UART_TIMEOUT_MS)
  {
    fault_raise(FAULT_UART_LOST);
  }
}

/**
  * @brief  Признак активной аварии.
  */
uint8_t fault_is_active(void)
{
  return (fault_code != FAULT_NONE) ? 1U : 0U;
}

/**
  * @brief  Код аварии (для телеметрии).
  */
uint8_t fault_get_code(void)
{
  return fault_code;
}

/**
  * @brief  Сброс аварии (команда X: стоп, затем R — запуск).
  */
void fault_clear(void)
{
  __disable_irq();
  fault_code = FAULT_NONE;
  stall_ms = 0U;
  __enable_irq();
  bemf_reset_invalid_streak();
}

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Аварийное отключение: ШИМ = 0, интегратор сброшен, LED горит.
  */
static void fault_raise(uint8_t code)
{
  if (fault_code == FAULT_NONE)
  {
    fault_code = code;
    motor_stop();                       /* Аварийное отключение ШИМ            */
    pid_reset();                        /* Сброс интегратора ПИ                */
    led_state = 1U;
    FAULT_LED_ON();
  }
}
