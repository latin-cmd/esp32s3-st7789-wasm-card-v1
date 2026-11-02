$ErrorActionPreference = 'Stop'
$hostDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $hostDir
Write-Host 'ESP32 Card Studio: http://localhost:8765'
Start-Process 'http://localhost:8765'
python -m http.server 8765 --bind 127.0.0.1
