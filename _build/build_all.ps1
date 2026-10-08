# ============================================================================
#  build_all.ps1 — сборка ОБЕИХ прошивок BEMF_reg из одного дерева исходников:
#    uart — управление приводом с ПК по USART1 (как раньше);
#    grbl — «шпиндель» для grblHAL: PB6 = ШИМ (уставка), PB8 = EN (пуск/стоп).
#
#  Запуск:  powershell -ExecutionPolicy Bypass -File _build\build_all.ps1
#  Результат:
#    _build\out\uart\BEMF_reg.{elf,bin,hex,map}
#    _build\out\grbl\BEMF_reg.{elf,bin,hex,map}
#    firmware\BEMF_reg_uart.{hex,bin}   <- готово к коммиту и к прошивке
#    firmware\BEMF_reg_grbl.{hex,bin}
# ============================================================================
$ErrorActionPreference = 'Stop'

$ROOT = 'c:\project\BEMF_reg'
$FW   = "$ROOT\firmware"

New-Item -ItemType Directory -Force -Path $FW | Out-Null

foreach ($mode in @('uart', 'grbl')) {
  Write-Host "=== сборка варианта $mode ==="
  & powershell -ExecutionPolicy Bypass -NoProfile -File "$ROOT\_build\build_main.ps1" -Mode $mode
  if ($LASTEXITCODE -ne 0) { throw "Сборка варианта $mode не удалась" }
}

foreach ($mode in @('uart', 'grbl')) {
  Copy-Item "$ROOT\_build\out\$mode\BEMF_reg.hex" "$FW\BEMF_reg_$mode.hex" -Force
  Copy-Item "$ROOT\_build\out\$mode\BEMF_reg.bin" "$FW\BEMF_reg_$mode.bin" -Force
}

Write-Host '== firmware\ =='
Get-ChildItem $FW | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
Write-Host 'OK: обе прошивки собраны, файлы для коммита — в firmware\'
