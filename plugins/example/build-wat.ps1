$ErrorActionPreference = 'Stop'

$pluginDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $pluginDir 'plugin.wat'
$output = Join-Path $pluginDir 'card_plugin.wasm'

wat2wasm $source -o $output
Write-Host "Built $output"
