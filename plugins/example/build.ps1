$ErrorActionPreference = 'Stop'

$pluginDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $pluginDir 'plugin.c'
$output = Join-Path $pluginDir 'card_plugin.wasm'

clang --target=wasm32 -Oz -nostdlib `
    -Wl,--no-entry `
    -Wl,--allow-undefined `
    -Wl,--strip-all `
    -Wl,--export=plugin_init `
    -Wl,--export=plugin_render `
    -o $output $source

Write-Host "Built $output"
