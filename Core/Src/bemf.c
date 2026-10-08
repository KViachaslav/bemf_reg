/**
  ******************************************************************************
  * @file    bemf.c
  * @brief   Дифференциальное измерение обратной ЭДС (BEMF) двумя каналами ADC1
  *          в измерительных паузах ШИМ: V_bemf = V_supply - V_low.
  ******************************************************************************
  *
  *  СХЕМА ПОДКЛЮЧЕНИЯ (правка ТЗ — см. popravka.md):
  *
  *      +12В ──────┬───────────────┐
  *                 │               │
  *              [МОТОР]         [диод SR340]
  *                 │            (катод к +12В)
  *                 │               │
  *                 ├───────► PA0 (ADC1_IN0) — низкая сторона мотора
  *                 │          (делитель 10к/2к на GND)
  *                 │
  *                 ├── сток IRLZ44N
  *                 │     исток → GND
  *                 │     затвор ← PA8 (TIM1_CH1) через 100 Ом
  *                 │
  *                 └───► PA1 (ADC1_IN1) — плюс мотора (питание)
  *                        (делитель 10к/2к на GND)
  *
  *  КЛЮЧЕВАЯ ИДЕЯ:
  *   - PA1 измеряет напряжение питания мотора V_supply (номинал 12 В);
  *   - PA0 измеряет напряжение на низкой стороне мотора V_low (там сидит BEMF);
  *   - BEMF = V_supply - V_low (в паузе, когда IRLZ44N закрыт).
  *  Питание измеряется в каждом замере, поэтому жёсткая константа 12 В в коде
  *  НЕ используется: просадка питания под нагрузкой компенсируется сама.
  *
  *  ЗАМЕР (период BEMF_MEASURE_PERIOD_MS):
  *   1. ШИМ останавливается, ключ IRLZ44N закрыт      — motor_pwm_pause();
  *   2. пауза BEMF_SETTLE_US — затухание индуктивного выброса;
  *   3. ADC1 в режиме Scan (IN0 → IN1) по программному триггеру, результаты
  *      перекладываются в память через DMA1 Channel1 (Circular);
  *   4. пересчёт кодов в V_low/V_supply, проверка достоверности;
  *   5. V_bemf = V_supply - V_low, скользящее среднее, RPM = V_bemf*K*sign;
  *   6. ШИМ возобновляется с прежней скважностью      — motor_pwm_resume().
  *
  *  Измерительная пауза блокирует прерывание TIM3 (~1 мс раз в 20 мс). Это
  *  осознанное решение из ТЗ: ШИМ на время паузы выключен, а ПИ-регулятор
  *  (TIM2, приоритет ниже) задерживается на 1 мс раз в 20 мс, что для контура
  *  1 кГц допустимо.
  */

/* Includes ------------------------------------------------------------------*/
#include "bemf.h"
#include "motor_pwm.h"

/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef h_bemf_tim;               /* TIM3: 1 мс, планировщик пауз    */
DMA_HandleTypeDef h_bemf_dma;               /* DMA1 Channel1: ADC1 -> память   */

static ADC_HandleTypeDef h_bemf_adc;        /* ADC1: 2 канала, SW-триггер      */

/* Буфер DMA: [0] — PA0 (низкая сторона), [1] — PA1 (питание).
   Тип uint16_t обязателен: DMA1 Channel1 настроен на полуслова
   (DMA_MDATAALIGN_HALFWORD), поэтому АЦП кладёт оба 12-битных отсчёта в
   буфер как две 16-битные ячейки. С типом uint32_t пара отсчётов
   упаковывалась в adc_dma_buf[0], а adc_dma_buf[1] оставался нулём, и
   каждый замер браковался как «код в нуле» (на стенде — ложный FLT=2).   */
static uint16_t adc_dma_buf[BEMF_ADC_CHANNEL_COUNT];
static volatile uint8_t adc_seq_done = 0U;  /* DMA переложил оба канала        */
static volatile uint8_t adc_seq_error = 0U; /* Ошибка канала DMA               */

static uint16_t raw_ring[BEMF_AVG_DEPTH];   /* Кольцо кодов низкой стороны     */
static uint32_t raw_sum = 0U;               /* Сумма элементов raw_ring        */
static float    vbemf_ring[BEMF_AVG_DEPTH]; /* Кольцо значений V_bemf, В       */
static float    vbemf_sum = 0.0f;           /* Сумма элементов vbemf_ring      */
static uint8_t  avg_index = 0U;             /* Индекс для записи               */
static uint8_t  avg_count = 0U;             /* Заполнено элементов (<= глубины) */

static uint16_t adc_low_raw = 0U;           /* Последний код PA0               */
static float    v_low = 0.0f;               /* Напряжение низкой стороны, В    */
static float    v_supply = 0.0f;            /* Напряжение питания мотора, В    */
static float    bemf_volts = 0.0f;          /* V_bemf на моторе, В             */
static float    bemf_rpm = 0.0f;            /* Обороты по BEMF                 */
static float    k_motor = BEMF_K_MOTOR_DEFAULT;
static uint8_t  invalid_streak = 0U;        /* Недостоверных замеров подряд    */
static uint8_t  sensor_streak = 0U;         /* Замеров «оба входа в нуле»      */
static uint8_t  data_valid = 0U;            /* Есть достоверный замер с пуска  */
static uint16_t ms_counter = 0U;            /* Счётчик мс до следующего замера */
static uint32_t cycles_per_us = 0U;         /* Для микросекундной задержки(DWT)*/

/* Private function prototypes -----------------------------------------------*/
static void     bemf_delay_us(uint32_t us);
static uint8_t  bemf_adc_scan(void);
static void     bemf_invalidate(void);

/* Exported functions --------------------------------------------------------*/

/**
  * @brief  Инициализация ADC1 (IN0/PA0 + IN1/PA1, DMA) и TIM3 (период замеров).
  */
void bemf_init(void)
{
  RCC_PeriphCLKInitTypeDef periph_clk = {0};
  ADC_ChannelConfTypeDef channel_config = {0};

  /* --- Микросекундная задержка на базе счётчика циклов ядра ----------------
     HAL_Delay() в прерывании запрещён ТЗ, поэтому используем DWT->CYCCNT.   */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  cycles_per_us = SystemCoreClock / 1000000U;

  /* --- Тактирование ADC1: PCLK2/6 = 10.67 МГц (максимум для F1 — 14 МГц) -- */
  periph_clk.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  periph_clk.AdcClockSelection = RCC_ADCPCLK2_DIV6;
  if (HAL_RCCEx_PeriphCLKConfig(&periph_clk) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_RCC_ADC1_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();          /* DMA1 Channel1 обслуживает ADC1      */

  /* --- DMA1 Channel1: ADC1->DR -> adc_dma_buf[] (по 2 байта, Circular) -----
     Circular выбран осознанно: на F1 запрос DMA формируется по EOC, и в
     циклическом режиме канал не «закрывается» после первой пары замеров.  */
  h_bemf_dma.Instance = BEMF_DMA_INSTANCE;
  h_bemf_dma.Init.Direction = DMA_PERIPH_TO_MEMORY;
  h_bemf_dma.Init.PeriphInc = DMA_PINC_DISABLE;
  h_bemf_dma.Init.MemInc = DMA_MINC_ENABLE;
  h_bemf_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  h_bemf_dma.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
  h_bemf_dma.Init.Mode = DMA_CIRCULAR;
  h_bemf_dma.Init.Priority = DMA_PRIORITY_LOW;
  if (HAL_DMA_Init(&h_bemf_dma) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_LINKDMA(&h_bemf_adc, DMA_Handle, h_bemf_dma);

  /* Приоритет выше, чем у TIM3: ожидание окончания DMA идёт внутри
     измерительной паузы, поэтому прерывание канала DMA обязано её прервать */
  HAL_NVIC_SetPriority(BEMF_DMA_IRQn, BEMF_DMA_IRQ_PREEMPT, 0U);
  HAL_NVIC_EnableIRQ(BEMF_DMA_IRQn);

  /* --- ADC1: scan из двух каналов, программный триггер, без Continuous ----- */
  h_bemf_adc.Instance = ADC1;
  h_bemf_adc.Init.ScanConvMode = ADC_SCAN_ENABLE;
  h_bemf_adc.Init.ContinuousConvMode = DISABLE;
  h_bemf_adc.Init.DiscontinuousConvMode = DISABLE;
  h_bemf_adc.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  h_bemf_adc.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  h_bemf_adc.Init.NbrOfConversion = BEMF_ADC_CHANNEL_COUNT;
  if (HAL_ADC_Init(&h_bemf_adc) != HAL_OK)
  {
    Error_Handler();
  }

  /* 71.5 такта — увеличенное время выборки: конденсатор S/H успевает
     зарядиться после индуктивного выброса мотора                          */
  channel_config.SamplingTime = ADC_SAMPLETIME_71CYCLES_5;

  channel_config.Channel = BEMF_ADC_CHANNEL_LOW;    /* PA0: низкая сторона    */
  channel_config.Rank = BEMF_ADC_RANK_LOW;
  if (HAL_ADC_ConfigChannel(&h_bemf_adc, &channel_config) != HAL_OK)
  {
    Error_Handler();
  }

  channel_config.Channel = BEMF_ADC_CHANNEL_SUPPLY; /* PA1: питание мотора    */
  channel_config.Rank = BEMF_ADC_RANK_SUPPLY;
  if (HAL_ADC_ConfigChannel(&h_bemf_adc, &channel_config) != HAL_OK)
  {
    Error_Handler();
  }

  /* Калибровка ADC измерительного канала (штатная процедура для F1):
     уменьшает погрешность нуля перед первым преобразованием.               */
  if (HAL_ADCEx_Calibration_Start(&h_bemf_adc) != HAL_OK)
  {
    Error_Handler();
  }

  /* --- TIM3: 64 МГц / 64 / 1000 = 1 кГц (прерывание каждую 1 мс) ---------- */
  __HAL_RCC_TIM3_CLK_ENABLE();
  h_bemf_tim.Instance = TIM3;
  h_bemf_tim.Init.Prescaler = BEMF_TIM_PRESCALER;
  h_bemf_tim.Init.CounterMode = TIM_COUNTERMODE_UP;
  h_bemf_tim.Init.Period = BEMF_TIM_PERIOD;
  h_bemf_tim.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  h_bemf_tim.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&h_bemf_tim) != HAL_OK)
  {
    Error_Handler();
  }
  /* Приоритет выше, чем у ПИ-регулятора: во время измерительной паузы
     регулятор не должен отрабатывать по «протухшим» данным               */
  HAL_NVIC_SetPriority(TIM3_IRQn, 1U, 0U);
  HAL_NVIC_EnableIRQ(TIM3_IRQn);
  if (HAL_TIM_Base_Start_IT(&h_bemf_tim) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief  Тик TIM3 (1 мс): каждые BEMF_MEASURE_PERIOD_MS выполняем замер.
  * @note   Измерительная пауза + ожидание DMA блокируют прерывание ~1 мс раз
  *         в 20 мс — осознанное решение из ТЗ (см. комментарий к файлу).
  */
void bemf_isr_tick(void)
{
  float vbemf;
  uint16_t raw_low;
  uint16_t raw_supply;

  ms_counter++;
  if (ms_counter < BEMF_MEASURE_PERIOD_MS)
  {
    return;
  }
  ms_counter = 0U;

  if (motor_is_running() == 0U)
  {
    /* Мотор обесточен: ЭДС отсутствует, замер бессмысленен */
    adc_low_raw = 0U;
    v_low = 0.0f;
    v_supply = 0.0f;
    bemf_volts = 0.0f;
    bemf_rpm = 0.0f;
    invalid_streak = 0U;
    sensor_streak = 0U;
    data_valid = 0U;
    return;
  }

  /* 1. Измерительная пауза: ключ IRLZ44N закрыт, мотор вращается по инерции */
  motor_pwm_pause();

  /* 2. Ожидание затухания индуктивного выброса (BEMF_SETTLE_US = 1 мс) */
  bemf_delay_us(BEMF_SETTLE_US);

  /* 3. Сканирование IN0 (низкая сторона) и IN1 (питание) через DMA */
  if (bemf_adc_scan() == 0U)
  {
    motor_pwm_resume();
    bemf_invalidate();
    return;
  }

  /* 4. Возврат ШИМ с прежней скважностью */
  motor_pwm_resume();

  raw_low = adc_dma_buf[0];
  raw_supply = adc_dma_buf[1];
  adc_low_raw = raw_low;

  /* 5. Обрыв измерительного тракта: оба входа в нуле (использует fault.c) */
  if ((raw_low == 0U) && (raw_supply == 0U))
  {
    if (sensor_streak < 0xFFU)
    {
      sensor_streak++;
    }
  }
  else
  {
    sensor_streak = 0U;
  }

  /* 6. Недостоверный замер: код «упёрся» в край шкалы (0 или 4095) хотя бы
     на одном входе — признак обрыва/КЗ делителя; в среднее не попадает.   */
  if ((raw_low <= (uint16_t)BEMF_RAW_INVALID_LO) ||
      (raw_low >= (uint16_t)BEMF_RAW_INVALID_HI) ||
      (raw_supply <= (uint16_t)BEMF_RAW_INVALID_LO) ||
      (raw_supply >= (uint16_t)BEMF_RAW_INVALID_HI))
  {
    bemf_invalidate();
    return;
  }

  /* 7. Пересчёт кодов АЦП в вольты: делитель 10к/2к стоит на обоих входах */
  v_low = (float)raw_low * BEMF_VREF_VOLTS / BEMF_ADC_FULL_SCALE
          * ((BEMF_R1_OHM + BEMF_R2_OHM) / BEMF_R2_OHM);
  v_supply = (float)raw_supply * BEMF_VREF_VOLTS / BEMF_ADC_FULL_SCALE
             * ((BEMF_R1_OHM + BEMF_R2_OHM) / BEMF_R2_OHM);

  /* 8. BEMF = V_supply - V_low; проверка достоверности результата: ЭДС не
     может быть отрицательной или больше напряжения питания (допуск
     BEMF_VBEMF_MARGIN_V — шум и разброс делителей).                       */
  vbemf = v_supply - v_low;
  if ((vbemf < -BEMF_VBEMF_MARGIN_V) || (vbemf > (v_supply + BEMF_VBEMF_MARGIN_V)))
  {
    bemf_invalidate();
    return;
  }
  if (vbemf < 0.0f)
  {
    vbemf = 0.0f;                      /* Малый отрицательный шумовой выброс   */
  }
  invalid_streak = 0U;
  data_valid = 1U;

  /* 9. Скользящее среднее: V_bemf (рабочее значение) и код низкой стороны
     (отдаётся в телеметрию)                                               */
  vbemf_sum -= vbemf_ring[avg_index];
  vbemf_ring[avg_index] = vbemf;
  vbemf_sum += vbemf;

  raw_sum -= (uint32_t)raw_ring[avg_index];
  raw_ring[avg_index] = raw_low;
  raw_sum += raw_low;

  avg_index++;
  if (avg_index >= BEMF_AVG_DEPTH)
  {
    avg_index = 0U;
  }
  if (avg_count < BEMF_AVG_DEPTH)
  {
    avg_count++;
  }

  /* 10. RPM = V_bemf * K_motor * sign (направление вращения — BEMF_RPM_SIGN) */
  bemf_volts = vbemf_sum / (float)avg_count;
  bemf_rpm = bemf_volts * k_motor * BEMF_RPM_SIGN;
}

/**
  * @brief  Колбэк HAL: DMA переложил оба канала скана — данные готовы.
  * @note   Вызывается из DMA1_Channel1_IRQHandler (приоритет 0), т. е. может
  *         исполниться во время измерительной паузы внутри TIM3.
  */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1)
  {
    adc_seq_done = 1U;
  }
}

/**
  * @brief  Колбэк HAL: ошибка DMA — замер считаем недостоверным.
  */
void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1)
  {
    adc_seq_error = 1U;
  }
}

/**
  * @brief  Последний «сырой» код АЦП низкой стороны (PA0).
  */
uint16_t bemf_get_raw(void)
{
  return adc_low_raw;
}

/**
  * @brief  Скользящее среднее кода АЦП низкой стороны (телеметрия).
  */
uint16_t bemf_get_raw_average(void)
{
  if (avg_count == 0U)
  {
    return 0U;
  }
  return (uint16_t)(raw_sum / (uint32_t)avg_count);
}

/**
  * @brief  Чистая ЭДС мотора V_bemf = V_supply - V_low (усреднённая), В.
  */
float bemf_get_volts(void)
{
  return bemf_volts;
}

/**
  * @brief  Измеренное напряжение питания мотора (для проверки питания), В.
  */
float bemf_get_supply_volts(void)
{
  return v_supply;
}

/**
  * @brief  Обороты, рассчитанные по BEMF.
  */
float bemf_get_rpm(void)
{
  return bemf_rpm;
}

/**
  * @brief  Задать коэффициент мотора (команда калибровки C).
  */
void bemf_set_k_motor(float k)
{
  if (k > 0.0f)
  {
    k_motor = k;
  }
}

/**
  * @brief  Текущий коэффициент мотора.
  */
float bemf_get_k_motor(void)
{
  return k_motor;
}

/**
  * @brief  Число недостоверных замеров подряд (для fault-модуля).
  */
uint8_t bemf_get_invalid_streak(void)
{
  return invalid_streak;
}

/**
  * @brief  Число замеров подряд с нулём на обоих входах АЦП (обрыв тракта).
  */
uint8_t bemf_get_sensor_streak(void)
{
  return sensor_streak;
}

/**
  * @brief  Признак наличия достоверного замера с момента пуска мотора.
  * @note   Нужен fault.c, чтобы проверка питания не срабатывала до первого
  *         достоверного замера (v_supply ещё не измерено).
  */
uint8_t bemf_data_valid(void)
{
  return data_valid;
}

/**
  * @brief  Сброс счётчика недостоверных замеров.
  */
void bemf_reset_invalid_streak(void)
{
  invalid_streak = 0U;
}

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Блокирующая задержка в микросекундах на счётчике циклов DWT.
  * @param  us: длительность, мкс
  */
static void bemf_delay_us(uint32_t us)
{
  uint32_t start = DWT->CYCCNT;
  uint32_t ticks = us * cycles_per_us;

  while ((DWT->CYCCNT - start) < ticks)
  {
    /* Ожидание набора циклов */
  }
}

/**
  * @brief  Сканирование двух каналов ADC1 (IN0 -> IN1) с DMA.
  * @note   Ожидание ограничено счётчиком BEMF_DMA_GUARD_ITER (при 64 МГц это
  *         ~1.5 мс против ожидаемых 16 мкс скана — 100-кратный запас):
  *         HAL_GetTick() в прерывании может не тикать, поэтому таймаут HAL не
  *         применяем. Прерывание DMA1_Channel1 имеет приоритет выше TIM3
  *         (BEMF_DMA_IRQ_PREEMPT), поэтому устанавливает adc_seq_done прямо во
  *         время ожидания.
  * @retval 1U — данные обоих каналов лежат в adc_dma_buf, 0U — ошибка/таймаут
  */
static uint8_t bemf_adc_scan(void)
{
  uint32_t guard = BEMF_DMA_GUARD_ITER;

  adc_seq_done = 0U;
  adc_seq_error = 0U;

  /* Приведение к uint32_t * — требование прототипа HAL; размер элемента
     задаётся настройкой DMA (DMA_MDATAALIGN_HALFWORD), поэтому буфер
     16-битный, а длина — в полусловах (по одной ячейке на канал).       */
  if (HAL_ADC_Start_DMA(&h_bemf_adc, (uint32_t *)adc_dma_buf,
                        BEMF_ADC_CHANNEL_COUNT) != HAL_OK)
  {
    return 0U;
  }

  while ((adc_seq_done == 0U) && (adc_seq_error == 0U) && (guard > 0U))
  {
    guard--;
  }

  (void)HAL_ADC_Stop_DMA(&h_bemf_adc);

  return ((adc_seq_done != 0U) && (adc_seq_error == 0U)) ? 1U : 0U;
}

/**
  * @brief  Признак недостоверного замера: V_bemf/RPM не обновляются, растёт
  *         счётчик недостоверных замеров (его читает fault.c).
  */
static void bemf_invalidate(void)
{
  if (invalid_streak < 0xFFU)
  {
    invalid_streak++;
  }
}
