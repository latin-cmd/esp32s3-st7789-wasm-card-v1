[CmdletBinding()]
param(
    [ValidateSet('build', 'menuconfig', 'size', 'shell')]
    [string]$Action = 'build'
)

$ErrorActionPreference = 'Stop'
$projectDir = Split-Path -Parent $PSScriptRoot
$composeFile = Join-Path $projectDir 'compose.yaml'

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw 'Docker CLI is not on PATH. Install/start Docker Desktop and open a new terminal.'
}

$dockerArgs = @('compose', '--file', $composeFile, 'run', '--rm', '--no-deps')
if ($Action -in @('build', 'size')) {
    $dockerArgs += '-T'
}
$dockerArgs += 'idf'
if ($Action -eq 'shell') {
    $dockerArgs += 'bash'
} else {
    $dockerArgs += @('idf.py', '-B', 'build-docker', $Action)
}

& docker @dockerArgs
exit $LASTEXITCODE
