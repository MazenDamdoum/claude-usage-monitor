<#
.SYNOPSIS
  Shows the cloud relay status: paired phones, open links, upcoming alerts.
.DESCRIPTION
  -Test sends a test notification to every phone of yours.
  -Remove <id> unpairs a phone (the id is shown in the list), e.g. a friend's old install.
  -Log lists the last notifications sent: when, to whom, and exactly what they said.
#>
param([switch]$Test, [string]$Remove, [switch]$Log)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

if ($Test) {
    $r = Invoke-Cloud POST '/ingest/test' '{}'
    if ($r.subscribed -eq 0) { Write-Host 'No device has notifications enabled yet.' }
    else { Write-Host "Delivered to $($r.delivered) of $($r.subscribed) device(s)." }
    return
}

if ($Remove) {
    Invoke-Cloud POST '/ingest/unpair' (@{ id = $Remove } | ConvertTo-Json -Compress) | Out-Null
    Write-Host "Removed phone $Remove."
    return
}

$cfg = Get-CloudConfig
$h = Invoke-Cloud GET '/ingest/health'
if ($Log) {
    foreach ($n in $h.log) {
        $when = [DateTimeOffset]::FromUnixTimeMilliseconds([long]$n.at).LocalDateTime.ToString('ddd HH:mm')
        Write-Host ("{0}  to {1,-10} {2} | {3}" -f $when, $n.to, $n.title, $n.body)
    }
    if (-not $h.log) { Write-Host 'No notifications logged yet.' }
    return
}
$fmt = { param($t) if ($t -gt 0) { [DateTimeOffset]::FromUnixTimeSeconds([long]$t).LocalDateTime.ToString('ddd HH:mm') } else { 'not scheduled' } }
$age = if ($h.usageReceivedAt) { [int](([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() - $h.usageReceivedAt) / 1000) } else { $null }
Write-Host "URL:            $($cfg.url)"
Write-Host "Devices:        $($h.devices) paired, $($h.subscribed) with notifications"
Write-Host ("Last upload:    " + $(if ($null -eq $age) { 'never' } else { "$age s ago" }))
foreach ($p in $h.phones) {
    $when = ([DateTimeOffset]::Parse($p.pairedAt)).LocalDateTime.ToString('ddd d MMM HH:mm')
    Write-Host ("  phone for {0,-8} paired {1}, notifications {2}  (id {3})" -f $p.for, $when, $(if ($p.notifications) { 'on' } else { 'off' }), $p.id)
}
foreach ($l in $h.links) {
    $until = [DateTimeOffset]::FromUnixTimeMilliseconds([long]$l.expiresAt).LocalDateTime.ToString('ddd HH:mm')
    Write-Host ("  open link for {0}, until {1}" -f $l.for, $until)
}
if ($h.resets) {
    foreach ($r in ($h.resets | Sort-Object at)) { Write-Host ("  {0,-8} {1,-6} resets {2}" -f $r.for, $r.limit, (& $fmt $r.at)) }
} else {
    Write-Host ("5-hour alert:   " + (& $fmt $h.schedule.five_hour_reset))
    Write-Host ("Weekly alert:   " + (& $fmt $h.schedule.weekly_reset))
}
