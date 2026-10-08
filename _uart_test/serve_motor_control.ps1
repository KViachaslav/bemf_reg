# ============================================================================
#  serve_motor_control.ps1 — локальный веб-сервер для пульта motor_control.html.
#
#  Зачем сервер: Web Serial API (navigator.serial) требует защищённого контекста.
#  Chrome/Edge считают доверенными https, http://localhost и ЛОКАЛЬНЫЕ ФАЙЛЫ —
#  поэтому пульт работает и при обычном двойном щелчке по motor_control.html
#  (file://). Сервер — удобство: постоянный адрес http://localhost:8099/,
#  авто-открытие браузера и предсказуемое поведение в старых версиях браузеров,
#  где file:// доверенным не считался. Скрипт отдаёт каталог _uart_test\.
#
#  Примеры:
#    powershell -ExecutionPolicy Bypass -File _uart_test\serve_motor_control.ps1
#    powershell -ExecutionPolicy Bypass -File _uart_test\serve_motor_control.ps1 -Port 8100
#    powershell -ExecutionPolicy Bypass -File _uart_test\serve_motor_control.ps1 -NoBrowser
#
#  Остановить: Ctrl+C в этом окне (сервер слушает только localhost, наружу
#  ничего не публикуется).
# ============================================================================
[CmdletBinding()]
param(
  [int]$Port = 8099,
  [string]$Page = 'motor_control.html',
  [switch]$NoBrowser
)

$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$page = Join-Path $root $Page
if (-not (Test-Path $page)) {
  throw ("Не найден файл пульта: {0}" -f $page)
}

function Get-BrowserPath {
  $candidates = @(
    (Join-Path $env:ProgramFiles 'Google\Chrome\Application\chrome.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Google\Chrome\Application\chrome.exe'),
    (Join-Path $env:LOCALAPPDATA 'Google\Chrome\Application\chrome.exe'),
    (Join-Path $env:ProgramFiles 'Microsoft\Edge\Application\msedge.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Microsoft\Edge\Application\msedge.exe')
  )
  foreach ($c in $candidates) {
    if ($c -and (Test-Path $c)) { return $c }
  }
  return $null
}

$url = 'http://localhost:{0}/{1}' -f $Port, $Page
$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add(('http://localhost:{0}/' -f $Port))

try {
  $listener.Start()
} catch {
  throw ("Не удалось занять порт {0}: {1}. Задайте другой, например -Port 8100." -f $Port, $_.Exception.Message)
}

$mime = @{
  '.html' = 'text/html; charset=utf-8'
  '.css'  = 'text/css; charset=utf-8'
  '.js'   = 'application/javascript; charset=utf-8'
  '.txt'  = 'text/plain; charset=utf-8'
  '.md'   = 'text/markdown; charset=utf-8'
  '.ps1'  = 'text/plain; charset=utf-8'
}

Write-Host ('Пульт мотора: {0}' -f $url)
Write-Host 'В диалоге подключения Web Serial выберите свой COM-порт (USB-SERIAL CH340 и т.п.).'
Write-Host 'Остановить сервер: Ctrl+C.'
Write-Host ''

if (-not $NoBrowser) {
  $browser = Get-BrowserPath
  if ($browser) {
    Write-Host ('Открываю {0}' -f $browser)
    Start-Process -FilePath $browser -ArgumentList $url | Out-Null
  } else {
    Write-Host 'Chrome/Edge не найден в стандартных путях — открываю браузер по умолчанию.'
    Start-Process $url | Out-Null
  }
}

try {
  while ($listener.IsListening) {
    $ctx = $listener.GetContext()
    $path = $ctx.Request.Url.LocalPath.TrimStart('/')
    if ([string]::IsNullOrWhiteSpace($path)) { $path = $Page }

    $target = Join-Path $root $path
    $inside = $target.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)
    $status = 200
    $type = 'application/octet-stream'

    if ($inside -and (Test-Path $target -PathType Leaf)) {
      $ext = [System.IO.Path]::GetExtension($target).ToLowerInvariant()
      if ($mime.ContainsKey($ext)) { $type = $mime[$ext] }
      $body = [System.IO.File]::ReadAllBytes($target)
    } else {
      $status = 404
      $type = 'text/plain; charset=utf-8'
      $body = [System.Text.Encoding]::UTF8.GetBytes('404 — файл не найден')
    }

    $ctx.Response.StatusCode = $status
    $ctx.Response.ContentType = $type
    $ctx.Response.ContentLength64 = $body.Length
    $ctx.Response.OutputStream.Write($body, 0, $body.Length)
    $ctx.Response.OutputStream.Close()

    Write-Host ('{0,-4} {1,-22} -> {2}' -f $ctx.Request.HttpMethod, $ctx.Request.Url.AbsolutePath, $status)
  }
} finally {
  $listener.Stop()
  $listener.Close()
  Write-Host 'Сервер остановлен.'
}
