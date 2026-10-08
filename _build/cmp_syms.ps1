# cmp_syms.ps1 — сравнение размеров символов двух ELF (для регресса вариантов).
# Запуск: powershell -File _build\cmp_syms.ps1 -A <elf> -B <elf> -Out <txt>
# ВНИМАНИЕ: в PowerShell имена переменных нечувствительны к регистру, поэтому
# параметры $A/$B нельзя присваивать напрямую — локальные имена другие.
param(
  [Parameter(Mandatory=$true)][string]$A,
  [Parameter(Mandatory=$true)][string]$B,
  [Parameter(Mandatory=$true)][string]$Out
)
$TOOL = 'C:\ST\STM32CubeIDE_2.0.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.100.202509120712\tools'
$NM   = "$TOOL\bin\arm-none-eabi-nm.exe"

function Load-Syms([string]$elf) {
  $h = @{}
  foreach ($l in (& $NM --format=posix --print-size --radix=d $elf)) {
    $p = $l -split '\s+'
    if ($p.Count -ge 4) { $h[$p[0]] = [int]$p[3] }
  }
  return $h
}

$symsA = Load-Syms $A
$symsB = Load-Syms $B
$lines = New-Object System.Collections.Generic.List[string]

foreach ($k in ($symsA.Keys | Sort-Object)) {
  if (-not $symsB.ContainsKey($k)) { $lines.Add(("ONLY-IN-A  {0} = {1}" -f $k, $symsA[$k])) }
  elseif ($symsA[$k] -ne $symsB[$k]) { $lines.Add(("SIZE-DIFF  {0} : {1} -> {2}" -f $k, $symsA[$k], $symsB[$k])) }
}
foreach ($k in ($symsB.Keys | Sort-Object)) {
  if (-not $symsA.ContainsKey($k)) { $lines.Add(("ONLY-IN-B  {0} = {1}" -f $k, $symsB[$k])) }
}
$lines.Add(("TOTALS: A={0} bytes in {1} syms, B={2} bytes in {3} syms" -f
            (($symsA.Values | Measure-Object -Sum).Sum), $symsA.Count,
            (($symsB.Values | Measure-Object -Sum).Sum), $symsB.Count))
$lines | Out-File -Encoding utf8 $Out
