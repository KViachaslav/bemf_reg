/**
  ******************************************************************************
  * @file    uart_cmd.c
  * @brief   Разбор команд ПК по USART1 и вывод телеметрии раз в 100 мс.
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "uart_cmd.h"
#include "motor_pwm.h"
#include "bemf.h"
#include "pid.h"
#include "fault.h"
#include "pwm_in.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Private variables ---------------------------------------------------------*/
extern UART_HandleTypeDef huart1;       /* Сконфигурирован CubeMX (MX_USART1_UART_Init) */

static uint8_t  rx_byte = 0U;           /* Одиночный байт приёма по прерыванию   */
static char     line_buf[UART_CMD_LINE_MAX];
static volatile uint8_t line_len = 0U;
static volatile uint8_t line_ready = 0U;
static volatile uint32_t last_cmd_tick = 0U;
static uint32_t telemetry_tick = 0U;
static float    cal_volts = 0.0f;       /* V_bemf, замеренное при калибровке     */

/* Private function prototypes -----------------------------------------------*/
static void uart_cmd_on_byte(uint8_t byte);
static void uart_cmd_execute(const char *line);
static void uart_cmd_calibrate(const char *argument);
static void uart_cmd_reply_error(const char *reason);

/* Exported functions --------------------------------------------------------*/

/**
  * @brief  Инициализация приёма команд (прерывание по 1 байту).
  */
void uart_cmd_init(void)
{
  line_len = 0U;
  line_ready = 0U;
  last_cmd_tick = HAL_GetTick();
  telemetry_tick = HAL_GetTick();
  (void)HAL_UART_Receive_IT(&huart1, &rx_byte, 1U);
}

/**
  * @brief  Обработка главного цикла: разбор готовой строки + телеметрия 100 мс.
  */
void uart_cmd_poll(void)
{
  if (line_ready != 0U)
  {
    uart_cmd_execute(line_buf);
    line_ready = 0U;                    /* Приём новой строки разблокирован     */
  }

  if ((HAL_GetTick() - telemetry_tick) >= UART_CMD_TELEMETRY_MS)
  {
    telemetry_tick = HAL_GetTick();
    uart_cmd_send_telemetry();
  }
}

/**
  * @brief  Время, прошедшее с момента последней принятой команды.
  */
uint32_t uart_cmd_ms_since_last(void)
{
  return HAL_GetTick() - last_cmd_tick;
}

/**
  * @brief  Блокирующая отправка строки (вызывается только из главного цикла).
  */
void uart_cmd_send_string(const char *text)
{
  size_t length = strlen(text);

  if (length > 0U)
  {
    (void)HAL_UART_Transmit(&huart1, (const uint8_t *)text, (uint16_t)length,
                            UART_CMD_TX_TIMEOUT_MS);
  }
}

/**
  * @brief  Колбэк HAL: принят 1 байт — складываем в строку и перезапускаем приём.
  */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    uart_cmd_on_byte(rx_byte);
    (void)HAL_UART_Receive_IT(&huart1, &rx_byte, 1U);
  }
}

/**
  * @brief  Колбэк HAL: ошибка линии (framing/noise/overrun) сбрасывает приём
  *         в IT-режиме, поэтому без перезапуска связь с ПК «умирает».
  */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    __HAL_UART_CLEAR_OREFLAG(huart);    /* Перезапуск приёма после сбоя линии */
    __HAL_UART_CLEAR_FEFLAG(huart);
    __HAL_UART_CLEAR_NEFLAG(huart);
    __HAL_UART_CLEAR_PEFLAG(huart);
    huart->ErrorCode = HAL_UART_ERROR_NONE;
    line_len = 0U;                      /* Неполную строку отбрасываем        */
    line_ready = 0U;
    (void)HAL_UART_Receive_IT(&huart1, &rx_byte, 1U);
  }
}

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Накопление строки до '\n'/'\r' (контекст прерывания USART1).
  */
static void uart_cmd_on_byte(uint8_t byte)
{
  if (line_ready != 0U)
  {
    return;                             /* Предыдущая команда ещё не разобрана */
  }

  if ((byte == (uint8_t)'\n') || (byte == (uint8_t)'\r'))
  {
    if (line_len > 0U)
    {
      line_buf[line_len] = '\0';
      line_len = 0U;
      line_ready = 1U;
      last_cmd_tick = HAL_GetTick();    /* Сторожевой таймер связи             */
    }
    return;
  }

  if (line_len < (UART_CMD_LINE_MAX - 1U))
  {
    line_buf[line_len] = (char)byte;
    line_len++;
  }
  else
  {
    line_len = 0U;                      /* Слишком длинная строка — сброс      */
  }
}

/**
  * @brief  Разбор и выполнение принятой строки (главный цикл).
  */
static void uart_cmd_execute(const char *line)
{
  char msg[72];
  const char *argument = &line[1];

  switch (line[0])
  {
    case 'S':
    case 's':
#if BEMF_CTRL_GRBL
      /* В режиме grbl уставка приходит по ШИМ (PB6): S доступна после M0. */
      if (pwm_in_is_uart_override() == 0U)
      {
        uart_cmd_reply_error("GRBL MODE, SEND M0 FIRST");
        break;
      }
#endif

      /* Уставка оборотов: S1500 */
      pid_set_setpoint((float)atof(argument));
      pid_reset();
      (void)snprintf(msg, sizeof(msg), "OK SET=%lu\r\n",
                     (unsigned long)((uint32_t)(pid_get_setpoint() + 0.5f)));
      uart_cmd_send_string(msg);
      break;

    case 'K':
    case 'k':
      /* Коэффициенты ПИ: K0.5,0.1 */
      {
        const char *comma = strchr(argument, ',');

        if (comma != NULL)
        {
          char kp_text[16];
          size_t head = (size_t)(comma - argument);
          if (head >= sizeof(kp_text))
          {
            head = sizeof(kp_text) - 1U;
          }
          (void)memcpy(kp_text, argument, head);
          kp_text[head] = '\0';
          pid_set_gains((float)atof(kp_text), (float)atof(comma + 1));
          (void)snprintf(msg, sizeof(msg), "OK K=%s,%s\r\n", kp_text, comma + 1);
          uart_cmd_send_string(msg);
        }
        else
        {
          uart_cmd_reply_error("FORMAT K<kp>,<ki>");
        }
      }
      break;

    case 'R':
    case 'r':
#if BEMF_CTRL_GRBL
      /* В режиме grbl пуском распоряжается вход разрешения PB8: ручной пуск
         возможен только после M0 (иначе мотор пошёл бы мимо задания).      */
      if (pwm_in_is_uart_override() == 0U)
      {
        uart_cmd_reply_error("GRBL MODE, SEND M0 FIRST");
        break;
      }
#endif

      /* Запуск */
      if (fault_is_active() != 0U)
      {
        uart_cmd_reply_error("FAULT ACTIVE, SEND X");
      }
      else
      {
        pid_reset();
        motor_start();
        uart_cmd_send_string("OK RUN\r\n");
      }
      break;

    case 'X':
    case 'x':
      /* Стоп + сброс аварии */
#if BEMF_CTRL_GRBL
      pwm_in_operator_stop();           /* Стоп + запрет автозапуска от PB8    */
#else
      motor_stop();
      pid_reset();
#endif
      fault_clear();
      uart_cmd_send_string("OK STOP\r\n");
      break;

    case '?':
      uart_cmd_send_telemetry();
      break;

    case 'C':
    case 'c':
#if BEMF_CTRL_GRBL
      /* Калибровка крутит мотор в открытом цикле — только в ручном режиме. */
      if (pwm_in_is_uart_override() == 0U)
      {
        uart_cmd_reply_error("GRBL MODE, SEND M0 FIRST");
        break;
      }
#endif

      uart_cmd_calibrate(argument);
      break;

#if BEMF_CTRL_GRBL
    case 'M':
    case 'm':
      /* M0 — ручной режим: уставка и пуск/стоп с ПК; M1 — обратно к grbl
         (уставка из скважности PB6, пуск/стоп по PB8).                     */
      if (argument[0] == '0')
      {
        pwm_in_set_uart_override(1U);
        uart_cmd_send_string("OK M0 UART MODE\r\n");
      }
      else if (argument[0] == '1')
      {
        pwm_in_set_uart_override(0U);
        uart_cmd_send_string("OK M1 GRBL MODE\r\n");
      }
      else
      {
        uart_cmd_reply_error("FORMAT M0|M1");
      }
      break;

    case 'P':
    case 'p':
      /* Обороты, соответствующие скважности 100 % — согласование шкалы с
         настройками grbl ($$): $30 = максимум об/мин, $36 = 100 %.         */
      if (argument[0] != '\0')
      {
        pwm_in_set_rpm_at_100((float)atof(argument));
      }
      (void)snprintf(msg, sizeof(msg), "OK P=%lu\r\n",
                     (unsigned long)((uint32_t)(pwm_in_get_rpm_at_100() + 0.5f)));
      uart_cmd_send_string(msg);
      break;
#endif

    default:
      uart_cmd_reply_error("UNKNOWN CMD");
      break;
  }
}

/**
  * @brief  Строка телеметрии: RPM, уставка, скважность, ошибка, АЦП низкой
  *         стороны, измеренное питание мотора и код аварии. В сборке GRBL
  *         дополнительно скважность/частота входного ШИМ и уровень PB8.
  */
void uart_cmd_send_telemetry(void)
{
#if BEMF_CTRL_GRBL
  char msg[128];                        /* Хватает на строку с PWM/F/EN       */
#else
  char msg[96];                         /* Строка варианта UART (как раньше)  */
#endif
  float rpm = bemf_get_rpm();
  float setpoint = pid_get_setpoint();
  float duty = motor_get_duty_percent();
  float supply = bemf_get_supply_volts();
  uint32_t duty_x10 = (uint32_t)((duty * 10.0f) + 0.5f);
  uint32_t supply_x10 = (uint32_t)((supply * 10.0f) + 0.5f);
  int32_t error_rpm = (int32_t)(setpoint - rpm);

#if BEMF_CTRL_GRBL
  /* Сборка GRBL: добавляем состояние входа управления (PB6/PB8). */
  {
    uint32_t pwm_x100 = pwm_in_duty_x100();
    (void)snprintf(msg, sizeof(msg),
                   "RPM=%lu SET=%lu DUTY=%lu.%lu%% ERR=%ld ADC=%u VS=%lu.%luV "
                   "PWM=%lu.%lu%% F=%lu EN=%u FLT=%u\r\n",
                   (unsigned long)((uint32_t)(rpm + 0.5f)),
                   (unsigned long)((uint32_t)(setpoint + 0.5f)),
                   (unsigned long)(duty_x10 / 10U),
                   (unsigned long)(duty_x10 % 10U),
                   (long)error_rpm,
                   (unsigned)bemf_get_raw_average(),
                   (unsigned long)(supply_x10 / 10U),
                   (unsigned long)(supply_x10 % 10U),
                   (unsigned long)(pwm_x100 / 100U),
                   (unsigned long)(pwm_x100 % 100U),
                   (unsigned long)pwm_in_freq_hz(),
                   (unsigned)pwm_in_enable_level(),
                   (unsigned)fault_get_code());
  }
#else
  (void)snprintf(msg, sizeof(msg),
                 "RPM=%lu SET=%lu DUTY=%lu.%lu%% ERR=%ld ADC=%u VS=%lu.%luV FLT=%u\r\n",
                 (unsigned long)((uint32_t)(rpm + 0.5f)),
                 (unsigned long)((uint32_t)(setpoint + 0.5f)),
                 (unsigned long)(duty_x10 / 10U),
                 (unsigned long)(duty_x10 % 10U),
                 (long)error_rpm,
                 (unsigned)bemf_get_raw_average(),
                 (unsigned long)(supply_x10 / 10U),
                 (unsigned long)(supply_x10 % 10U),
                 (unsigned)fault_get_code());
#endif
  uart_cmd_send_string(msg);
}

/**
  * @brief  Ответ об ошибке команды.
  */
static void uart_cmd_reply_error(const char *reason)
{
  uart_cmd_send_string("ERR ");
  uart_cmd_send_string(reason);
  uart_cmd_send_string("\r\n");
}

/**
  * @brief  Калибровка коэффициента K_motor.
  * @param  argument: пусто — прогон при 50 % ШИМ и замер V_bemf;
  *                   число — показания внешнего тахометра для пересчёта K.
  * @note   На время прогона ПИ-регулятор отключён (открытый цикл) и
  *         главный цикл занят ожиданием, но обмен и защиты (прерывания)
  *         продолжают работать.
  */
static void uart_cmd_calibrate(const char *argument)
{
  char msg[72];
  uint32_t start_tick;
  uint32_t millivolts;
  uint32_t k_x10;
  float rpm_reference;

  if (*argument != '\0')
  {
    /* C<rpm> — второй шаг: оператор ввёл обороты, снятые внешним тахометром */
    rpm_reference = (float)atof(argument);
    if ((rpm_reference <= 0.0f) || (cal_volts <= 0.0f))
    {
      uart_cmd_reply_error("NO CAL DATA, SEND C FIRST");
      return;
    }
    bemf_set_k_motor(rpm_reference / cal_volts);
    k_x10 = (uint32_t)((bemf_get_k_motor() * 10.0f) + 0.5f);
    (void)snprintf(msg, sizeof(msg), "OK K=%lu.%lu\r\n",
                   (unsigned long)(k_x10 / 10U), (unsigned long)(k_x10 % 10U));
    uart_cmd_send_string(msg);
    return;
  }

  if (fault_is_active() != 0U)
  {
    uart_cmd_reply_error("FAULT ACTIVE, SEND X");
    return;
  }
  if (motor_is_running() != 0U)
  {
    uart_cmd_reply_error("STOP MOTOR FIRST (X)");
    return;
  }

  /* Первый шаг: открытый цикл, 50 % ШИМ, контролируемое время прогона */
  pid_set_enabled(0U);
  pid_reset();
  motor_start();
  motor_set_duty(UART_CMD_CAL_DUTY);

  start_tick = HAL_GetTick();
  while ((HAL_GetTick() - start_tick) < (uint32_t)UART_CMD_CAL_TIME_MS)
  {
    /* Ожидание установившихся оборотов: замеры BEMF делает TIM3. */
  }

  cal_volts = bemf_get_volts();

  motor_stop();
  pid_reset();
  pid_set_enabled(1U);                  /* Возврат в замкнутый контур          */

  if (fault_is_active() != 0U)
  {
    uart_cmd_reply_error("FAULT DURING CAL");
    return;
  }

  millivolts = (uint32_t)((cal_volts * 1000.0f) + 0.5f);
  (void)snprintf(msg, sizeof(msg), "OK CAL VBEMF=%lu.%03lu V, SEND C<rpm>\r\n",
                 (unsigned long)(millivolts / 1000U),
                 (unsigned long)(millivolts % 1000U));
  uart_cmd_send_string(msg);
}
