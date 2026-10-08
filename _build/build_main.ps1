# ============================================================================
#  build_main.ps1 — сборка БОЕВОЙ прошивки BEMF_reg (Core + HAL) без IDE.
#
#  Тулчейн — GNU Tools for STM32 из состава STM32CubeIDE (13.3.rel1), тот же,
#  что описан в README §6.4. Флаги совпадают с проектными: -Wall -Wextra -O0.
#
#  Запуск:  powershell -ExecutionPolicy Bypass -File _build\build_main.ps1
#           powershell -ExecutionPolicy Bypass -File _build\build_main.ps1 -Mode grbl
#  Результат: _build\out\uart\BEMF_reg.elf  либо  _build\out\grbl\BEMF_reg.elf
#             (+ .bin, .hex, .map; копия последней сборки — в _build\out\).
#  Вариант задаётся -Mode uart|grbl (макрос BEMF_CTRL_GRBL, см. ctrl_mode.h):
#  uart — управление с ПК (как раньше), grbl — шпиндель grblHAL: PB6 = ШИМ,
#  PB8 = разрешение вращения (README §18). Собрать оба — _build\build_all.ps1.
#
#  ВАЖНО (подводные камни, проверено вживую):
#   1) каждый аргумент gcc котируем — иначе PowerShell искажает токены
#      вида `-Tscript.ld`, `-Wl,--gc-sections`, `-specs=nano.specs`;
#   2) `-specs=nano.specs` в этом паке работает только по АБСОЛЮТНОМУ пути;
#   3) пути к объектным файлам передаём через response-файл gcc (@objs.rsp),
#      чтобы не зависеть от того, как PowerShell разворачивает массив.
# ============================================================================
param(
  [ValidateSet('uart', 'grbl')]
  [string]$Mode = 'uart'                # uart — управление с ПК, grbl — вход ШИМ+EN
)

$ErrorActionPreference = 'Stop'

$TOOL    = 'C:\ST\STM32CubeIDE_2.0.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712\tools'
$GCC     = "$TOOL\bin\arm-none-eabi-gcc.exe"
$OBJCOPY = "$TOOL\bin\arm-none-eabi-objcopy.exe"
$SIZE    = "$TOOL\bin\arm-none-eabi-size.exe"
$NM      = "$TOOL\bin\arm-none-eabi-nm.exe"
$NANO    = "$TOOL\arm-none-eabi\lib\nano.specs"

$ROOT = 'c:\project\BEMF_reg'
$OUT  = "$ROOT\_build\out\$Mode"
$OUTROOT = "$ROOT\_build\out"
$LD   = "$ROOT\STM32F103C8TX_FLASH.ld"
$STUP = "$ROOT\Core\Startup\startup_stm32f103c8tx.s"
$ELF  = "$OUT\BEMF_reg.elf"
$BIN  = "$OUT\BEMF_reg.bin"
$HEX  = "$OUT\BEMF_reg.hex"
$MAP  = "$OUT\BEMF_reg.map"
$RSP  = "$OUT\objs.rsp"

# Управление режимом задаёт компилятору ключ BEMF_CTRL_GRBL (см. ctrl_mode.h):
#   0 — сборка UART (как раньше), 1 — сборка GRBL (PB6 = ШИМ, PB8 = EN).
$DEFS = if ($Mode -eq 'grbl') { @('-DBEMF_CTRL_GRBL=1') } else { @('-DBEMF_CTRL_GRBL=0') }

if (-not (Test-Path $GCC)) { throw "Не найден тулчейн: $GCC" }

Remove-Item -Recurse -Force $OUT -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $OUT | Out-Null

$SRCS = @(Get-ChildItem "$ROOT\Core\Src\*.c") + `
        @(Get-ChildItem "$ROOT\Drivers\STM32F1xx_HAL_Driver\Src\*.c")

Write-Host ("== compile: {0} файлов ==" -f $SRCS.Count)

$failed = 0
foreach ($f in $SRCS) {
  & $GCC $DEFS '-DUSE_HAL_DRIVER' '-DSTM32F103xB' `
         "-I$ROOT\Core\Inc" `
         "-I$ROOT\Drivers\STM32F1xx_HAL_Driver\Inc" `
         "-I$ROOT\Drivers\STM32F1xx_HAL_Driver\Inc\Legacy" `
         "-I$ROOT\Drivers\CMSIS\Device\ST\STM32F1xx\Include" `
         "-I$ROOT\Drivers\CMSIS\Include" `
         '-mcpu=cortex-m3' '-mthumb' '-Wall' '-Wextra' '-O0' `
         '-ffunction-sections' '-fdata-sections' '-std=gnu11' '-c' `
         $f.FullName '-o' "$OUT\$($f.BaseName).o"
  if ($LASTEXITCODE -ne 0) { Write-Host "FAILED: $($f.Name)"; $failed++ }
}
if ($failed -ne 0) { throw "$failed файл(ов) не скомпилировались" }

Write-Host '== link BEMF_reg.elf =='
$objs = @(Get-ChildItem "$OUT\*.o" | Where-Object { $_.BaseName -ne 'startup_stm32f103c8tx' } |
          ForEach-Object { $_.FullName })
$objs += $STUP
# ВНИМАНИЕ: в response-файлах gcc обратный слэш — escape-символ, поэтому
# 'c:\project\...' превращается в 'c:project...'. Пишем прямые слэши и кавычки.
$rspLines = $objs | ForEach-Object { '"' + $_.Replace('\', '/') + '"' }
Set-Content -Path $RSP -Value $rspLines -Encoding ASCII

& $GCC '-mcpu=cortex-m3' '-mthumb' "-T$LD" "-specs=$NANO" `
       '-Wl,--gc-sections' "-Wl,-Map=$MAP" `
       "@$RSP" '-lc' '-lm' '-lnosys' '-o' $ELF
if ($LASTEXITCODE -ne 0) { throw 'Линковка не удалась' }

& $OBJCOPY -O binary $ELF $BIN
& $OBJCOPY -O ihex   $ELF $HEX

# Копия «последней собранной» прошивки в _build\out — для скриптов и README,
# которые ссылаются на путь без варианта сборки (по умолчанию — uart).
Copy-Item $ELF "$OUTROOT\BEMF_reg.elf" -Force
Copy-Item $BIN "$OUTROOT\BEMF_reg.bin" -Force
Copy-Item $HEX "$OUTROOT\BEMF_reg.hex" -Force

& $SIZE $ELF

Write-Host '== проверка векторов прерываний =='
& $NM $ELF | Select-String 'DMA1_Channel1_IRQHandler|TIM3_IRQHandler|TIM2_IRQHandler|TIM4_IRQHandler'

Write-Host "OK ($Mode) -> $ELF"
