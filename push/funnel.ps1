<#
.SYNOPSIS
  Publishes the phone app over HTTPS with Tailscale Funnel, on port 10000 only.
  Your other Funnel routes (443, 8443) are not touched. Only the public app port
  is exposed; the Windows app's internal port never is.
.EXAMPLE
  .\funnel.ps1 -Enable     # turn on and print the iPhone URL
  .\funnel.ps1 -Disable    # turn off port 10000
  .\funnel.ps1             # show status
#>
param([switch]$Enable, [switch]$Disable)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$ts = Get-Tailscale
if (-not $ts) { throw 'Tailscale is not installed: https://tailscale.com/download/windows' }

if ($Enable) {
    & $ts funnel --bg "--https=$FunnelPort" "http://127.0.0.1:$PublicPort"
    if ($LASTEXITCODE -ne 0) { throw 'tailscale funnel failed (see the message above; Funnel may need enabling in your tailnet).' }
} elseif ($Disable) {
    & $ts funnel "--https=$FunnelPort" off
    if ($LASTEXITCODE -ne 0) { throw 'tailscale funnel off failed' }
    Write-Host "Funnel on port $FunnelPort is off."
    return
}

$url = Get-PushPublicUrl
if ($url) {
    Write-Host "Phone app: $url" -ForegroundColor Green
} else {
    Write-Host "Funnel is not serving the phone app (port $FunnelPort -> 127.0.0.1:$PublicPort)."
}
