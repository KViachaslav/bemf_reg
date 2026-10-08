/**
  ******************************************************************************
  * @file    ctrl_mode.h
  * @brief   Режим управления приводом — выбирается при сборке прошивки.
  *
  *          Из одного и того же дерева исходников собираются ДВЕ прошивки:
  *
  *          BEMF_CTRL_GRBL = 0  (UART, по умолчанию)
  *              Управление с ПК по USART1: S/K/R/X/?/C. Поведение, баннер и
  *              телеметрия совпадают с версией, описанной в README §8–§9.
  *              Модуль pwm_in в сборку не входит, TIM4 не используется.
  *
  *          BEMF_CTRL_GRBL = 1  (GRBL)
  *              Привод — «шпиндель» для grblHAL: уставка оборотов задаётся
  *              скважностью внешнего ШИМ (PB6 = TIM4_CH1, захват), разрешение
  *              вращения — уровнем на PB8 (1 = вращать, 0 = стоп, pull-down).
  *              Описание — README §18.
  *
  *          Сборка:  _build\build_main.ps1 -Mode grbl      (или -Mode uart)
  *                   _build\build_all.ps1                  (обе сразу)
  ******************************************************************************/

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __CTRL_MODE_H
#define __CTRL_MODE_H

/* Задаётся ключом компилятора -DBEMF_CTRL_GRBL=1; по умолчанию — UART      */
#ifndef BEMF_CTRL_GRBL
#define BEMF_CTRL_GRBL 0
#endif

#if (BEMF_CTRL_GRBL != 0) && (BEMF_CTRL_GRBL != 1)
#error "BEMF_CTRL_GRBL: допустимы только 0 (UART) и 1 (GRBL)"
#endif

#if BEMF_CTRL_GRBL
#define BEMF_FW_NAME "BEMF_reg grbl"
#else
#define BEMF_FW_NAME "BEMF_reg"
#endif

#endif /* __CTRL_MODE_H */
