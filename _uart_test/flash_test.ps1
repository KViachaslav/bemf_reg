# ============================================================================
#  flash_test.ps1 — прошивка тестовой UART-прошивки в STM32F103 через ST-Link.
#  Требуется: ST-Link по SWD + STM32CubeProgrammer.
#  Запуск:  powershell -ExecutionPolicy Bypass -File _uart_test\flash_test.ps1
# ============================================================================
$ErrorActionPreference = 'Stop'

$PROG = 'C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe'
$ELF  = 'c:\project\BEMF_reg\_uart_test\build\uart_test.elf'

if (-not (Test-Path $ELF)) { throw "Сначала соберите прошивку: _uart_test\build_test.ps1" }

& $PROG -c port=SWD -w $ELF -v -rst
Write-Host "Прошивка залита и выполнен сброс (MCU Reset)."
