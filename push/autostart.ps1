<#
.SYNOPSIS
  Optionally start the push server when you sign in to Windows (per-user, no admin).
.EXAMPLE
  .\autostart.ps1 -Enable
  .\autostart.ps1 -Disable
#>
param([switch]$Enable, [switch]$Disable)
$ErrorActionPreference = 'Stop'
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$name = 'ClaudeUsagePushServer'

if ($Enable) {
    $start = Join-Path $PSScriptRoot 'start.ps1'
    $cmd = "powershell.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$start`" -Background"
    Set-ItemProperty -Path $key -Name $name -Value $cmd
    Write-Host 'The push server will start when you sign in.'
} elseif ($Disable) {
    Remove-ItemProperty -Path $key -Name $name -ErrorAction SilentlyContinue
    Write-Host 'Autostart removed.'
} else {
    $v = (Get-ItemProperty -Path $key -Name $name -ErrorAction SilentlyContinue).$name
    if ($v) { Write-Host "Autostart is on: $v" } else { Write-Host 'Autostart is off.' }
}
