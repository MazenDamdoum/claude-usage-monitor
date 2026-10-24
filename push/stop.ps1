<#
.SYNOPSIS
  Stops the background push server.
#>
$ErrorActionPreference = 'Stop'
$server = (Join-Path $PSScriptRoot 'server.js').ToLower()
$procs = Get-CimInstance Win32_Process -Filter "Name = 'node.exe'" |
    Where-Object { $_.CommandLine -and $_.CommandLine.ToLower().Contains($server) }
if (-not $procs) {
    Write-Host 'Push server is not running.'
    return
}
$procs | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
Write-Host "Stopped push server ($(@($procs).Count) process)."
