<#
.SYNOPSIS
  Lets Claude Usage Monitor switch accounts the moment Claude Code stops on a usage limit.
.DESCRIPTION
  The monitor normally switches before a limit is reached, so Claude Code carries on without
  stopping. As a safety net, this adds a StopFailure hook to Claude Code's user settings
  (%USERPROFILE%\.claude\settings.json): when a turn ends on a rate limit, Claude Code runs
  `Claude Usage Monitor.exe --limit-hit` and the monitor switches to the next account at once.
  (Claude Code ignores this hook's output, so after such a stop you still send one message.)

  Your settings.json is backed up first, and running this again replaces the old entry.
  -Remove takes the hook out again.
.EXAMPLE
  .\scripts\install-claude-hook.ps1
#>
param([string]$Exe, [switch]$Remove)
$ErrorActionPreference = 'Stop'

if (-not $Exe) {
    $running = Get-Process | Where-Object { $_.Path -and (Split-Path $_.Path -Leaf) -match '^(Claude Usage Monitor|ClaudeUsageMonitor)\.exe$' } | Select-Object -First 1
    if ($running) { $Exe = $running.Path }
}
if (-not $Remove -and (-not $Exe -or -not (Test-Path $Exe))) {
    throw 'Start Claude Usage Monitor first, or pass -Exe "C:\path\to\Claude Usage Monitor.exe".'
}

$dir = if ($env:CLAUDE_CONFIG_DIR) { $env:CLAUDE_CONFIG_DIR } else { Join-Path $env:USERPROFILE '.claude' }
$file = Join-Path $dir 'settings.json'
$settings = if (Test-Path $file) { [IO.File]::ReadAllText($file) | ConvertFrom-Json } else { [pscustomobject]@{} }
if (Test-Path $file) { Copy-Item $file "$file.bak-$(Get-Date -Format yyyyMMdd-HHmmss)" }

if (-not $settings.PSObject.Properties['hooks']) { $settings | Add-Member hooks ([pscustomobject]@{}) }
$hooks = $settings.hooks
$entries = @()
if ($hooks.PSObject.Properties['StopFailure']) {
    # Keep everyone else's StopFailure hooks; drop only ours.
    $entries = @($hooks.StopFailure | Where-Object { -not (@($_.hooks | Where-Object { $_.command -match '--limit-hit' }).Count) })
}
if (-not $Remove) {
    $command = '"' + ($Exe -replace '\\', '/') + '" --limit-hit'
    $entries += [pscustomobject]@{ matcher = 'rate_limit'; hooks = @([pscustomobject]@{ type = 'command'; command = $command; timeout = 10 }) }
}
if ($entries.Count) {
    if ($hooks.PSObject.Properties['StopFailure']) { $hooks.StopFailure = $entries } else { $hooks | Add-Member StopFailure $entries }
} elseif ($hooks.PSObject.Properties['StopFailure']) {
    $hooks.PSObject.Properties.Remove('StopFailure')
}

$json = ConvertTo-Json -InputObject $settings -Depth 32
$null = $json | ConvertFrom-Json  # still valid JSON, or nothing is written
New-Item -ItemType Directory -Force $dir | Out-Null
[IO.File]::WriteAllText($file, $json, (New-Object Text.UTF8Encoding($false)))
if ($Remove) { Write-Host 'Removed the limit hook from Claude Code.' }
else { Write-Host "Claude Code will tell Claude Usage Monitor when it hits a usage limit ($file)." }
