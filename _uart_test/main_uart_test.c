/*
 ******************************************************************************
 * @file    main_uart_test.c
 * @brief   ТЕСТОВАЯ прошивка: проверка аппаратного тракта UART на STM32F103C8T6.
 ******************************************************************************
 *
 *  НАЗНАЧЕНИЕ
 *  ----------
 *  Минимальная автономная прошивка (без HAL, без мотора, без ADC/TIM), чтобы
 *  проверить ТОЛЬКО линию UART: MCU (PA9/PA10) <-> USB-UART (CH340) <-> ПК.
 *
 *  Что проверяется:
 *    - PA9  = USART1_TX  — уходит баннер + периодический "PING"
 *    - PA10 = USART1_RX  — всё принятое возвращается эхом (echo mode)
 *    - PC13 = LED (active low) мигает ~1 Гц  — признак, что МК жив
 *    - Тактирование HSI 8 МГц -> PLL x16 -> 64 МГц (как в основной прошивке),
 *      поэтому скорость 115200 совпадает с боевой конфигурацией.
 *
 *  ПОДКЛЮЧЕНИЕ (как в README основной прошивки)
 *  --------------------------------------------
 *      PA9  (TX) -> RXD адаптера CH340
 *      PA10 (RX) <- TXD адаптера CH340
 *      GND       <-> GND адаптера    (обязательно!)
 *      Терминал: 115200 бод, 8 бит, без чётности, 1 стоп-бит, без flow control.
 *
 *  ОЖИДАЕМЫЙ РЕЗУЛЬТАТ
 *  -------------------
 *  В терминале сразу после сброса:
 *      === BEMF_reg UART TEST ===
 *      USART1 PA9(TX)/PA10(RX) 115200 8N1, SYSCLK=64MHz (HSI/2 x16)
 *      Echo mode: everything you type is sent back.
 *      LED PC13 blinks ~1Hz.
 *      PING 0  uptime=0 ms  [PA9->CH340->PC]
 *      PING 1  uptime=500 ms ...
 *  Каждый набранный символ должен вернуться эхом -> значит работает и RX.
 *
 *  ЭТА ПРОШИВКА НЕ ВХОДИТ В ОСНОВНУЮ СБОРКУ (папка _uart_test/).
 ******************************************************************************
 */

#include "stm32f1xx.h"

/* Частота после PLL: HSI 8 МГц / 2 * 16 = 64 МГц */
#define SYSCLK_HZ              64000000U

/* BRR для 115200 при PCLK2 = 64 МГц:
   USARTDIV = 64e6 / (16 * 115200) = 34.72 -> mantissa=34, frac=0.72*16=11 -> 0x22B */
#define USART1_BRR_115200_64M  0x22BU

#define LED_PIN                13U          /* PC13, active low            */
#define MS_PER_TICK            1U           /* шаг цикла, мс               */

/* Private function prototypes -----------------------------------------------*/
static void clock_64mhz(void);
static void gpio_init(void);
static void usart1_init(void);
static void systick_init(void);
static void delay_ms(uint32_t ms);
static void uart_putc(char c);
static void uart_puts(const char *s);
static void uart_putu(uint32_t value);
static int  uart_getc(void);

/* ---------------------------------------------------------------------------
 * SystemInit() вызывается из startup_stm32f103c8tx.s сразу после сброса.
 * Здесь пусто — часы настраиваются в clock_64mhz() уже в main().
 * ------------------------------------------------------------------------- */
void SystemInit(void)
{
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
  uint32_t pings = 0U;
  uint32_t uptime_ms = 0U;
  int c;

  clock_64mhz();
  gpio_init();
  usart1_init();
  systick_init();

  uart_puts("\r\n=== BEMF_reg UART TEST ===\r\n");
  uart_puts("USART1 PA9(TX)/PA10(RX) 115200 8N1, SYSCLK=64MHz (HSI/2 x16)\r\n");
  uart_puts("Echo mode: everything you type is sent back.\r\n");
  uart_puts("LED PC13 blinks ~1Hz.\r\n\r\n");

  for (;;)
  {
    /* --- RX: эхо каждого принятого байта (проверка PA10) --- */
    c = uart_getc();
    if (c >= 0)
    {
      uart_putc((char)c);
    }

    /* --- TX: раз в 500 мс счётчик + мигание LED (проверка PA9) --- */
    if ((uptime_ms % 500U) == 0U)
    {
      uart_puts("PING ");
      uart_putu(pings);
      uart_puts("  uptime=");
      uart_putu(uptime_ms);
      uart_puts(" ms  [PA9->CH340->PC]\r\n");
      pings++;

      /* LED active low: каждые 500 мс меняем уровень -> мигание 1 Гц */
      if ((GPIOC->ODR & (1U << LED_PIN)) != 0U)
      {
        GPIOC->BRR = (1U << LED_PIN);       /* низкий -> светодиод горит     */
      }
      else
      {
        GPIOC->BSRR = (1U << LED_PIN);      /* высокий -> светодиод погашен  */
      }
    }

    delay_ms(MS_PER_TICK);
    uptime_ms += MS_PER_TICK;
  }
}

/* ---------------------------------------------------------------------------
 * Тактирование: HSI 8 МГц -> PLL(HSI/2 x16) -> SYSCLK 64 МГц.
 * APB1 = 32 МГц, APB2 = 64 МГц (USART1 тактируется от APB2).
 * ------------------------------------------------------------------------- */
static void clock_64mhz(void)
{
  /* Flash: prefetch + 2 wait states (обязательно ДО разгона выше 24 МГц). */
  FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;

  /* HSI включён после сброса — убеждаемся, что он готов. */
  RCC->CR |= RCC_CR_HSION;
  while ((RCC->CR & RCC_CR_HSIRDY) == 0U)
  {
  }

  /* PLL: источник HSI/2 (4 МГц), множитель 16 -> 64 МГц. */
  RCC->CFGR &= ~(RCC_CFGR_PLLMULL | RCC_CFGR_PLLSRC | RCC_CFGR_PLLXTPRE);
  RCC->CFGR |= RCC_CFGR_PLLMULL16;
  RCC->CR |= RCC_CR_PLLON;
  while ((RCC->CR & RCC_CR_PLLRDY) == 0U)
  {
  }

  /* Шины: AHB=/1, APB1=/2 (32 МГц), APB2=/1 (64 МГц). */
  RCC->CFGR &= ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2);
  RCC->CFGR |= RCC_CFGR_PPRE1_DIV2;

  /* Переключаем SYSCLK на PLL и ждём подтверждения. */
  RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
  while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL)
  {
  }
}

/* ---------------------------------------------------------------------------
 * GPIO: PA9 (TX, AF push-pull), PA10 (RX, input floating), PC13 (LED out).
 * ------------------------------------------------------------------------- */
static void gpio_init(void)
{
  /* Такты GPIOA, GPIOC, AFIO, USART1 (все на APB2). */
  RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPCEN |
                  RCC_APB2ENR_AFIOEN | RCC_APB2ENR_USART1EN;

  /* PA9 = USART1_TX: alternate function push-pull, 50 МГц -> CNF=10, MODE=11 */
  GPIOA->CRH = (GPIOA->CRH & ~(0xFU << 4)) | (0xBU << 4);

  /* PA10 = USART1_RX: input floating -> CNF=01, MODE=00 */
  GPIOA->CRH = (GPIOA->CRH & ~(0xFU << 8)) | (0x4U << 8);

  /* PA8 = затвор IRLZ44N: выход push-pull 2 МГц, принудительно НИЗКИЙ.
     В тесте ШИМ не используется, поэтому ключ должен быть гарантированно
     ЗАКРЫТ (важно: диод Шоттки на плате не установлен -> без разряда затвора
     возможен «плавающий» ключ). ШИМ в основной прошивке вернёт PA8 к TIM1_CH1. */
  GPIOA->CRH = (GPIOA->CRH & ~(0xFU << 0)) | (0x2U << 0);
  GPIOA->BRR = (1U << 8);

  /* PC13 = LED: output push-pull, 2 МГц -> CNF=00, MODE=10 */
  GPIOC->CRH = (GPIOC->CRH & ~(0xFU << 20)) | (0x2U << 20);
  GPIOC->BSRR = (1U << LED_PIN);            /* по умолчанию LED погашен      */
}

/* ---------------------------------------------------------------------------
 * USART1: 115200 8N1, TX+RX включены, без прерываний (опрос).
 * ------------------------------------------------------------------------- */
static void usart1_init(void)
{
  USART1->BRR = USART1_BRR_115200_64M;
  USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

/* ---------------------------------------------------------------------------
 * SysTick: источник — ядро 64 МГц, период 1 мс.
 * ------------------------------------------------------------------------- */
static void systick_init(void)
{
  SysTick->LOAD = (SYSCLK_HZ / 1000U) - 1U; /* 64000 - 1 -> 1 мс            */
  SysTick->VAL  = 0U;
  SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
}

/* ---------------------------------------------------------------------------
 * Блокирующая задержка на SysTick (COUNTFLAG сбрасывается чтением CTRL).
 * ------------------------------------------------------------------------- */
static void delay_ms(uint32_t ms)
{
  while (ms-- > 0U)
  {
    while ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) == 0U)
    {
    }
  }
}

/* ---------------------------------------------------------------------------
 * Отправка одного байта (ждём пустого регистра данных).
 * ------------------------------------------------------------------------- */
static void uart_putc(char c)
{
  while ((USART1->SR & USART_SR_TXE) == 0U)
  {
  }
  USART1->DR = (uint16_t)(uint8_t)c;
}

/* ---------------------------------------------------------------------------
 * Отправка строки.
 * ------------------------------------------------------------------------- */
static void uart_puts(const char *s)
{
  while (*s != '\0')
  {
    uart_putc(*s);
    s++;
  }
}

/* ---------------------------------------------------------------------------
 * Отправка беззнакового числа (без printf — чтобы прошивка была крошечной).
 * ------------------------------------------------------------------------- */
static void uart_putu(uint32_t value)
{
  char digits[10];
  uint32_t i = 0U;

  if (value == 0U)
  {
    uart_putc('0');
    return;
  }

  while (value > 0U)
  {
    digits[i] = (char)('0' + (value % 10U));
    value /= 10U;
    i++;
  }

  while (i > 0U)
  {
    i--;
    uart_putc(digits[i]);
  }
}

/* ---------------------------------------------------------------------------
 * Неблокирующее чтение байта: возвращает 0..255 или -1, если данных нет.
 * ------------------------------------------------------------------------- */
static int uart_getc(void)
{
  if ((USART1->SR & USART_SR_RXNE) == 0U)
  {
    return -1;
  }
  return (int)(USART1->DR & 0xFFU);
}
