# Shared helpers for the push scripts. Dot-source: . (Join-Path $PSScriptRoot 'common.ps1')

$PushDataDir = if ($env:CUM_PUSH_DATA) { $env:CUM_PUSH_DATA } else { Join-Path $env:APPDATA 'ClaudeUsageMonitor\push' }
$PushLog = Join-Path $PushDataDir 'server.log'
$FunnelPort = 10000   # the only Funnel port this feature uses (443 and 8443 are left alone)
$PublicPort = if ($env:CUM_PUSH_PORT) { [int]$env:CUM_PUSH_PORT } else { 47823 }

function Get-Tailscale {
    $cmd = Get-Command tailscale -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $default = Join-Path $env:ProgramFiles 'Tailscale\tailscale.exe'
    if (Test-Path $default) { return $default }
    return $null
}

function Get-PushBridge {
    $file = Join-Path $PushDataDir 'bridge.json'
    if (-not (Test-Path $file)) { return $null }
    return Get-Content $file -Raw | ConvertFrom-Json
}

function Test-PushServerRunning {
    $bridge = Get-PushBridge
    if (-not $bridge) { return $false }
    try {
        $r = Invoke-RestMethod -Uri "http://127.0.0.1:$($bridge.port)/internal/health" -Headers @{ Authorization = "Bearer $($bridge.token)" } -TimeoutSec 2
        return [bool]$r.ok
    } catch {
        return $false
    }
}

function Invoke-PushInternal([string]$Path, [string]$Body = '{}') {
    $bridge = Get-PushBridge
    if (-not $bridge -or -not (Test-PushServerRunning)) { throw 'The push server is not running. Start it with push\start.ps1 -Background' }
    return Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$($bridge.port)$Path" -Headers @{ Authorization = "Bearer $($bridge.token)" } -ContentType 'application/json' -Body $Body -TimeoutSec 30
}

# The public HTTPS address of the phone app, if Funnel is serving it.
function Get-PushPublicUrl {
    $ts = Get-Tailscale
    if (-not $ts) { return $null }
    $status = & $ts funnel status 2>$null | Out-String
    $m = [regex]::Match($status, "(https://[^\s]+:$FunnelPort)\s*\(Funnel on\)\s*\r?\n\|-- / proxy http://127\.0\.0\.1:$PublicPort")
    if ($m.Success) { return $m.Groups[1].Value }
    return $null
}
