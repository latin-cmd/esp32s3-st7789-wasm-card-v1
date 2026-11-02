[CmdletBinding()]
param(
    [ValidateSet('up', 'down', 'logs', 'status')]
    [string]$Action = 'up',
    [string]$Address
)
$ErrorActionPreference = 'Stop'
$projectDir = Split-Path -Parent $PSScriptRoot
$composeFile = Join-Path $projectDir 'compose.yaml'
if ($Action -eq 'up') {
    if (-not $Address) {
        $candidates = @(Get-NetIPConfiguration | Where-Object {
            $_.IPv4DefaultGateway -and $_.IPv4Address
        } | ForEach-Object { $_.IPv4Address.IPAddress } | Where-Object {
            $_ -notmatch '^(127\.|169\.254\.)'
        } | Select-Object -Unique)
        if ($candidates.Count -ne 1) {
            throw 'Please specify the Wi-Fi/LAN IPv4 address: .\tools\wireless.ps1 up -Address 192.168.x.x'
        }
        $Address = $candidates[0]
    }
    $parsed = $null
    if (-not [Net.IPAddress]::TryParse($Address, [ref]$parsed) -or
        $parsed.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) {
        throw 'Address must be the computer IPv4 address reachable from the ESP32.'
    }
    $env:CARD_PUBLIC_URL = "http://${Address}:8787"
    $env:CARD_MQTT_URL = "mqtt://${Address}:1883"
    # Persist the advertised LAN endpoints across later compose rebuilds.
    $envFile = Join-Path $projectDir '.env'
    $settings = @()
    if (Test-Path -LiteralPath $envFile) {
        $settings = @(Get-Content -LiteralPath $envFile | Where-Object {
            $_ -notmatch '^CARD_(PUBLIC|MQTT)_URL='
        })
    }
    $settings += "CARD_PUBLIC_URL=$env:CARD_PUBLIC_URL"
    $settings += "CARD_MQTT_URL=$env:CARD_MQTT_URL"
    [IO.File]::WriteAllLines($envFile, [string[]]$settings, [Text.UTF8Encoding]::new($false))
    & docker compose --file $composeFile up --build --detach wireless
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host "Browser: http://localhost:8787"
    Write-Host "ESP32 setup / management service: $env:CARD_PUBLIC_URL"
    Write-Host 'Local development only. Use HTTPS/WSS/MQTT TLS before internet deployment.'
} elseif ($Action -eq 'down') {
    # Keep all tenant/device/image data in the named volume.
    & docker compose --file $composeFile stop wireless
} elseif ($Action -eq 'logs') {
    & docker compose --file $composeFile logs --tail 100 wireless
} else {
    & docker compose --file $composeFile ps wireless
}
exit $LASTEXITCODE
