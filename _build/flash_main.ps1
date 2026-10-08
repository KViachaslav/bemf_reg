# ============================================================================
#  flash_main.ps1 — прошивка БОЕВОЙ прошивки BEMF_reg в STM32F103 через ST-Link.
#  Требуется: ST-Link по SWD + STM32CubeProgrammer.
#  Запуск:  powershell -ExecutionPolicy Bypass -File _build\flash_main.ps1
# ============================================================================
$ErrorActionPreference = 'Stop'

$PROG = 'C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe'
$ELF  = 'c:\project\BEMF_reg\_build\out\BEMF_reg.elf'

if (-not (Test-Path $ELF)) { throw 'Сначала соберите прошивку: powershell -File _build\build_main.ps1' }

& $PROG -c port=SWD -w $ELF -v -rst
Write-Host 'Прошивка залита, выполнен сброс (MCU Reset).'
