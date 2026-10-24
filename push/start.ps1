<#
.SYNOPSIS
  Starts the push server (installs its dependencies on first run).
.PARAMETER Background
  Run hidden in the background; it keeps running after this window or VS Code closes.
#>
param([switch]$Background)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) { throw 'Node.js 18 or newer is required: https://nodejs.org' }

if (-not (Test-Path (Join-Path $PSScriptRoot 'node_modules'))) {
    Write-Host 'Installing dependencies...'
    Push-Location $PSScriptRoot
    try { npm ci --omit=dev --no-fund --no-audit | Out-Host } finally { Pop-Location }
    if ($LASTEXITCODE -ne 0) { throw 'npm ci failed' }
}

if (Test-PushServerRunning) {
    Write-Host "Push server is already running (log: $PushLog)."
    return
}

$server = Join-Path $PSScriptRoot 'server.js'
if (-not $Background) {
    & $node.Source $server
    return
}

Start-Process -FilePath $node.Source -ArgumentList "`"$server`"" -WorkingDirectory $PSScriptRoot -WindowStyle Hidden
for ($i = 0; $i -lt 50 -and -not (Test-PushServerRunning); $i++) { Start-Sleep -Milliseconds 100 }
if (Test-PushServerRunning) {
    Write-Host "Push server started in the background (log: $PushLog)."
} else {
    throw "Push server did not start. See $PushLog"
}
