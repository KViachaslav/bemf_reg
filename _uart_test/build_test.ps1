# ============================================================================
#  build_test.ps1 — сборка тестовой прошивки проверки UART (main_uart_test.c)
#  Тулчейн берётся из состава установленной STM32CubeIDE (как в README).
#  Результат: _uart_test\build\uart_test.elf / .bin
#
#  Запуск:  powershell -ExecutionPolicy Bypass -File _uart_test\build_test.ps1
# ============================================================================
$ErrorActionPreference = 'Stop'

$TOOL    = 'C:\ST\STM32CubeIDE_2.0.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712\tools'
$GCC     = "$TOOL\bin\arm-none-eabi-gcc.exe"
$OBJCOPY = "$TOOL\bin\arm-none-eabi-objcopy.exe"
$SIZE    = "$TOOL\bin\arm-none-eabi-size.exe"
$NANO    = "$TOOL\arm-none-eabi\lib\nano.specs"

$ROOT = 'c:\project\BEMF_reg'
$OUT  = "$ROOT\_uart_test\build"
$SRC  = "$ROOT\_uart_test\main_uart_test.c"
$LD   = "$ROOT\STM32F103C8TX_FLASH.ld"
$STUP = "$ROOT\Core\Startup\startup_stm32f103c8tx.s"
$ELF  = "$OUT\uart_test.elf"
$BIN  = "$OUT\uart_test.bin"

Remove-Item -Recurse -Force $OUT -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $OUT | Out-Null

Write-Host '== compile main_uart_test.c =='
& $GCC '-DSTM32F103xB' `
       '-I.' `
       '-Ic:/project/BEMF_reg/Drivers/CMSIS/Device/ST/STM32F1xx/Include' `
       '-Ic:/project/BEMF_reg/Drivers/CMSIS/Include' `
       '-mcpu=cortex-m3' '-mthumb' '-Wall' '-Wextra' '-O2' `
       '-ffunction-sections' '-fdata-sections' '-std=gnu11' '-c' `
       $SRC '-o' "$OUT\main_uart_test.o"

Write-Host '== link uart_test.elf =='
& $GCC '-mcpu=cortex-m3' '-mthumb' `
       "-T$LD" "-specs=$NANO" '-Wl,--gc-sections' `
       "-Wl,-Map=$OUT\uart_test.map" `
       "$OUT\main_uart_test.o" $STUP `
       '-lc' '-lm' '-lnosys' '-o' $ELF

& $OBJCOPY -O binary $ELF $BIN
& $SIZE $ELF
Write-Host "OK -> $ELF"
