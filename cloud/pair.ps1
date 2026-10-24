<#
.SYNOPSIS
  Makes a personal link that installs the phone app and signs it in by itself.
.DESCRIPTION
  Without -Owner: a link for your own iPhone (every account, full control).
  With -Owner <name>: a link for the friend whose Claude account that is. Their phone sees
  only that account's usage, gets alerts about it, and can cap your use of it or ask you
  to stop.
  Each link works once, within 24 hours. A new link for the same person replaces the old one.
.EXAMPLE
  .\pair.ps1 -Owner alice
#>
param([string]$Owner)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$cfg = Get-CloudConfig
$body = if ($Owner) { @{ owner = $Owner } | ConvertTo-Json -Compress } else { '{}' }
try {
    $pair = Invoke-Cloud POST '/ingest/pair' $body
} catch {
    if ($Owner -and $_.Exception.Response -and [int]$_.Exception.Response.StatusCode -eq 404) {
        Write-Host "No account named '$Owner' has reached the cloud yet." -ForegroundColor Red
        Write-Host 'Check the name in the taskbar, keep Claude Usage Monitor running for a minute, and try again.'
        exit 1
    }
    throw
}
$link = "$($cfg.url)/join/$($pair.code)"
$expires = [DateTimeOffset]::FromUnixTimeMilliseconds([long]$pair.expiresAt).LocalDateTime
$who = if ($Owner) { $pair.owner } else { 'your iPhone' }
Write-Host ''
Write-Host "  Link for ${who}:" -ForegroundColor Cyan
Write-Host "    $link" -ForegroundColor Yellow
Write-Host ''
Write-Host '  Open it in Safari on the iPhone, tap Share > Add to Home Screen, open Claude Usage'
Write-Host '  from the Home Screen (it signs in by itself) and tap Enable Notifications.'
Write-Host ''
Write-Host "  Works once, until $($expires.ToString('ddd HH:mm'))."
Write-Host ''
