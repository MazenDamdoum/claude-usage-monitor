<#
.SYNOPSIS
  Sends a test notification to every paired device with notifications enabled.
.PARAMETER Reset
  Send a sample reset notification instead: FiveHour or Weekly.
.EXAMPLE
  .\test-notification.ps1
  .\test-notification.ps1 -Reset FiveHour
#>
param([ValidateSet('FiveHour', 'Weekly')][string]$Reset)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

if ($Reset) {
    $kind = if ($Reset -eq 'FiveHour') { 'five_hour_reset' } else { 'weekly_reset' }
    $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $body = @{ id = "${kind}:$now"; kind = $kind; reset_at = $now } | ConvertTo-Json -Compress
    Invoke-PushInternal '/internal/event' $body | Out-Null
    Write-Host "Sent a sample $Reset reset notification to every subscribed device."
    return
}

$result = Invoke-PushInternal '/internal/test'
if ($result.subscribed -eq 0) {
    Write-Host 'No device has notifications enabled yet. Pair your iPhone and tap Enable Notifications.'
} else {
    Write-Host "Delivered to $($result.delivered) of $($result.subscribed) device(s)."
}
