# ============================================================================
#  read_uart.ps1 — чтение тестового UART (115200 8N1) с USB-UART адаптера.
#
#  Примеры:
#    powershell -ExecutionPolicy Bypass -File _uart_test\read_uart.ps1 -LineTime
#    powershell -ExecutionPolicy Bypass -File _uart_test\read_uart.ps1 -Port COM11 -Seconds 10 -LineTime
#    powershell -ExecutionPolicy Bypass -File _uart_test\read_uart.ps1 -Port COM11 -Reset -Send -LineTime
#
#  Bluetooth-порты автоматически исключаются.
#  -LineTime  печатает каждую строку с меткой времени от начала захвата:
#             сразу видно, "живой" это эфир (метка ~= uptime) или старое
#             содержимое буфера драйвера.
#  -Reset     сбрасывает МК через ST-Link ПОСЛЕ открытия порта (ловим баннер).
#  -Send      отправляет строку (-SendText, по умолчанию HELLO) через
#             -SendAfterMs мс после старта; прошивка вернёт её эхом (проверка RX).
#  -TimedCmds расписание команд "<мс>:<текст>;...", например
#             -TimedCmds "1000:S1000;1500:R;7000:?;13000:?;16000:X".
#             Нужно для прогонов мотора: уставка + R, затем периодический '?'
#             (модуль fault глушит мотор через 5 с молчания ПК, FLT=3) и стоп X
#             — всё в одном захвате, без ручных скриптов между запусками.
# ============================================================================
[CmdletBinding()]
param(
  [string]$Port,
  [int]$Baud = 115200,
  [int]$Seconds = 10,
  [switch]$Send,
  [string]$SendText = 'HELLO',
  [int]$SendAfterMs = 2500,
  [int]$SendGapMs = 0,
  [switch]$LineTime,
  [int]$WaitSec = 0,
  [switch]$Reset,
  [string]$TimedCmds = ''
)

$ErrorActionPreference = 'Stop'

function Get-BluetoothPorts {
  try {
    return @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
             Where-Object { $_.Name -match 'Bluetooth' } |
             ForEach-Object { if ($_.Name -match '\((COM\d+)\)') { $Matches[1] } })
  } catch { return @() }
}

function Get-CandidatePorts {
  $bt = @(Get-BluetoothPorts)
  return @([System.IO.Ports.SerialPort]::GetPortNames() | Where-Object { $bt -notcontains $_ })
}

if (-not $Port) {
  # @() обязателен: при единственном порте PowerShell разворачивает массив,
  # и $cand[0] от строки "COM11" дал бы символ 'C'.
  $cand = @(Get-CandidatePorts)

  if ($cand.Count -eq 0 -and $WaitSec -gt 0) {
    Write-Host ("USB-UART порт ещё не найден. Ждём до {0} c — воткните адаптер..." -f $WaitSec)
    $swWait = [System.Diagnostics.Stopwatch]::StartNew()
    while ($swWait.Elapsed.TotalSeconds -lt $WaitSec -and $cand.Count -eq 0) {
      Start-Sleep -Milliseconds 500
      $cand = @(Get-CandidatePorts)
    }
  }

  if ($cand.Count -eq 0) {
    Write-Host 'USB-UART порт не найден (Bluetooth-порты исключены).'
    Write-Host 'Проверьте: адаптер подключён? драйвер CH340/CP210x установлен? кабель data, не charge-only?'
    exit 1
  }
  $Port = [string]$cand[0]
  if ($Port -notmatch '^COM\d+$') {
    Write-Host ("Не удалось определить порт (получено '{0}'). Укажите его явно: -Port COM11" -f $Port)
    exit 1
  }
}

Write-Host ("Порт: {0}   {1} 8N1   захват {2} c" -f $Port, $Baud, $Seconds)

$sp = New-Object System.IO.Ports.SerialPort($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$sp.ReadTimeout = 200
$sp.WriteTimeout = 500
$sp.Open()

$resetJob = $null
$ms   = New-Object System.IO.MemoryStream
$line = New-Object System.Text.StringBuilder
$sent = $false

# Разбор расписания -TimedCmds: [{"At"=<мс>; "Text"=<строка>; "Done"}]
$timed = @()
if ($TimedCmds) {
  foreach ($item in $TimedCmds.Split(';')) {
    if ($item -match '^\s*(\d+)\s*:\s*(.+?)\s*$') {
      $timed += [pscustomobject]@{ At = [int]$Matches[1]; Text = $Matches[2]; Done = $false }
    } else {
      Write-Host ("Расписание: пропущен элемент '{0}' (ожидается <мс>:<текст>)" -f $item)
    }
  }
  $timed = @($timed | Sort-Object At)
}

try {
  if ($Reset) {
    $prog = 'C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe'
    if (Test-Path $prog) {
      Write-Host '[+      0 ms] >>> RESET МК через ST-Link'
      $resetJob = Start-Job -ScriptBlock { param($p) & $p -c port=SWD -rst } -ArgumentList $prog
    } else {
      Write-Host 'STM32_Programmer_CLI.exe не найден — сброс пропущен.'
    }
  }

  $sw = [System.Diagnostics.Stopwatch]::StartNew()

  while ($sw.Elapsed.TotalSeconds -lt $Seconds) {

    if ($Send -and -not $sent -and $sw.Elapsed.TotalMilliseconds -ge $SendAfterMs) {
      $payload = $SendText + "`r`n"
      if ($SendGapMs -gt 0) {
        # По одному символу с паузой: так опрос в тестовой прошивке успевает
        # принять каждый байт (у STM32F1 в USART нет FIFO, приём по опросу).
        foreach ($ch in $payload.ToCharArray()) {
          $sp.Write([string]$ch)
          Start-Sleep -Milliseconds $SendGapMs
        }
      } else {
        $sp.Write($payload)
      }
      Write-Host ("[+{0,7} ms] >>> ОТПРАВЛЕНО: {1}" -f [int]$sw.Elapsed.TotalMilliseconds, $SendText)
      $sent = $true
    }
    foreach ($t in @($timed)) {
      if ((-not $t.Done) -and ($sw.Elapsed.TotalMilliseconds -ge $t.At)) {
        $t.Done = $true
        $sp.Write($t.Text + "`r`n")
        Write-Host ("[+{0,7} ms] >>> ОТПРАВЛЕНО: {1}" -f [int]$sw.Elapsed.TotalMilliseconds, $t.Text)
      }
    }



    try {
      $b = $sp.ReadByte()
      if ($b -lt 0) { continue }

      [void]$ms.WriteByte([byte]$b)

      $ch = [char]$b
      if ($ch -eq "`n") {
        $t = $line.ToString().TrimEnd("`r")
        if ($LineTime) { Write-Host ("[+{0,7} ms] {1}" -f [int]$sw.Elapsed.TotalMilliseconds, $t) }
        else           { Write-Host $t }
        [void]$line.Clear()
      } elseif ($ch -ne "`r") {
        [void]$line.Append($ch)
      }
    } catch [System.TimeoutException] { }
  }

  if ($line.Length -gt 0) {
    if ($LineTime) { Write-Host ("[+{0,7} ms] {1}" -f [int]$sw.Elapsed.TotalMilliseconds, $line.ToString()) }
    else           { Write-Host $line.ToString() }
  }
} finally {
  if ($sp.IsOpen) { $sp.Close() }
  if ($resetJob) {
    Wait-Job $resetJob -Timeout 20 | Out-Null
    Remove-Job $resetJob -Force -ErrorAction SilentlyContinue
  }
}

$bytes = $ms.ToArray()
Write-Host ''
Write-Host ('--- Принято байт: {0} ---' -f $bytes.Length)
Write-Host '--- HEX ---'
Write-Host (($bytes | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')
