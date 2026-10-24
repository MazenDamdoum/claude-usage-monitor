<#
.SYNOPSIS
  Creates a one-time pairing code (valid 10 minutes) for your iPhone.
#>
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$pair = Invoke-PushInternal '/internal/pair'
$expires = [DateTimeOffset]::FromUnixTimeMilliseconds([long]$pair.expiresAt).LocalDateTime
$url = Get-PushPublicUrl

Write-Host ''
Write-Host "  Pairing code:  $($pair.code)" -ForegroundColor Yellow
Write-Host "  Valid until:   $($expires.ToShortTimeString()) (one use)"
Write-Host ''
if ($url) {
    Write-Host "  On your iPhone, open $url in Safari,"
} else {
    Write-Host '  Funnel is not on yet: run push\funnel.ps1 -Enable first, then open the URL it prints in Safari,'
}
Write-Host '  add it to your Home Screen, open it from there and enter the code.'
Write-Host ''
