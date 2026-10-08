# firmware — готовые прошивки BEMF_reg

Здесь лежат **обе** готовые прошивки, собранные из одного дерева исходников
(`_build\build_all.ps1`). Их можно прошить без сборки:

```powershell
powershell -ExecutionPolicy Bypass -File _build\flash_main.ps1            # uart (по умолчанию)
powershell -ExecutionPolicy Bypass -File _build\flash_main.ps1 -Fw grbl
```

| Файл | Вариант | Что умеет |
|---|---|---|
| `BEMF_reg_uart.hex` / `.bin` | UART (`BEMF_CTRL_GRBL=0`) | Управление с ПК по USART1: `S<rpm>`, `K<kp>,<ki>`, `R`, `X`, `?`, `C`. Поведение полностью совпадает с версией, описанной в README §8–§9 |
| `BEMF_reg_grbl.hex` / `.bin` | GRBL (`BEMF_CTRL_GRBL=1`) | «Шпиндель» для grblHAL: уставка по ШИМ **PB6** (TIM4_CH1), пуск/стоп по **PB8**; команды `M0`/`M1`/`P`/`X`/`?` по USART1. Описание — README §18 |

Дополнительно в GRBL-варианте телеметрия расширена полями
`PWM=<входная скважность %> F=<частота Гц> EN=<уровень PB8>`.

Артефакты сборки (ELF/MAP/объектные файлы) в репозиторий не попадают — только
`.hex` и `.bin`. Пересобрать после правок:

```powershell
powershell -ExecutionPolicy Bypass -File _build\build_all.ps1
```

| Файл | Размер, байт | MD5 |
|---|---|---|
| `BEMF_reg_uart.bin` | 40880 | `0CF8580BA515614D08B07DEED24B67F9` |
| `BEMF_reg_uart.hex` | 115070 | `72F076966FDD80B8E46B652844B46277` |
| `BEMF_reg_grbl.bin` | 44636 | `559BC1EF837A97A6C21298E843E630CE` |
| `BEMF_reg_grbl.hex` | 125624 | `4020123382E0F36FBAD5667A8E2BDAFC` |

Размеры секций (`arm-none-eabi-size`): вариант `uart` — `text=40392 data=484
bss=2476`, вариант `grbl` — `text=44144 data=488 bss=2584`; компиляция без
предупреждений (`-Wall -Wextra`). Сборка воспроизводима: повторный прогон
`build_all.ps1` даёт те же MD5. Контрольные суммы удобно сверять после
пересборки (`Get-FileHash firmware\*.bin,firmware\*.hex -Algorithm MD5`).
