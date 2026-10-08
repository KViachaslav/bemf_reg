# ============================================================================
#  flash_main.ps1 — прошивка BEMF_reg в STM32F103 через ST-Link.
#  Требуется: ST-Link по SWD + STM32CubeProgrammer.
#
#  Запуск:  powershell -ExecutionPolicy Bypass -File _build\flash_main.ps1            (uart)
#           powershell -ExecutionPolicy Bypass -File _build\flash_main.ps1 -Fw grbl
#
#  Берётся собранный ELF (_build\out\<вариант>\BEMF_reg.elf), а если его нет —
#  готовый файл из firmware\BEMF_reg_<вариант>.hex (может быть скачан/склонирован
#  готовым, без сборки).
# ============================================================================
param(
  [ValidateSet('uart', 'grbl')]
  [string]$Fw = 'uart'
)

$ErrorActionPreference = 'Stop'

$PROG = 'C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe'
$ROOT = 'c:\project\BEMF_reg'
$ELF  = "$ROOT\_build\out\$Fw\BEMF_reg.elf"
$HEX  = "$ROOT\firmware\BEMF_reg_$Fw.hex"

if (Test-Path $ELF) {
  $TARGET = $ELF
} elseif (Test-Path $HEX) {
  $TARGET = $HEX
} else {
  throw "Нет прошивки для '$Fw': соберите её (_build\build_all.ps1) или положите firmware\BEMF_reg_$Fw.hex"
}
if (-not (Test-Path $PROG)) { throw "Не найден STM32_Programmer_CLI: $PROG" }

Write-Host "== прошивка варианта $Fw : $TARGET =="
& $PROG -c port=SWD -w $TARGET -v -rst
Write-Host 'Прошивка залита, выполнен сброс (MCU Reset).'

