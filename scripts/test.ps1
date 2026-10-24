<#
.SYNOPSIS
  Builds the app and runs every test: C++ (reset detection, push bridge) and,
  when Node.js is installed, the push server suite (including the C++ bridge
  talking to a real server).
#>
param(
    [switch]$SkipPush
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

& (Join-Path $PSScriptRoot 'build.ps1')

$zig = Get-ChildItem (Join-Path $root '.tools') -Recurse -Filter zig.exe | Select-Object -First 1 -ExpandProperty FullName
$src = Join-Path $root 'src'
$out = Join-Path $root 'build\push_bridge_test.exe'
Write-Host 'Building push_bridge_test.exe...'
& $zig c++ -target x86_64-windows-gnu -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter `
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -o $out `
    (Join-Path $root 'tests\push_bridge_test.cpp') `
    ('PushBridge.cpp', 'ResetDetector.cpp', 'AppFiles.cpp', 'Settings.cpp', 'UsageClient.cpp', 'Accounts.cpp', 'UsageView.cpp', 'PromptStats.cpp' | ForEach-Object { Join-Path $src $_ }) `
    -lwinhttp -ladvapi32
if ($LASTEXITCODE -ne 0) { throw 'C++ test build failed' }
Get-ChildItem (Join-Path $root 'build') -Filter *.pdb -ErrorAction SilentlyContinue | Remove-Item -Force

Write-Host "`n== C++ tests"
& $out
if ($LASTEXITCODE -ne 0) { throw 'C++ tests failed' }

if ($SkipPush) { return }
if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
    Write-Warning 'Node.js not found: skipping push server tests.'
    return
}
$push = Join-Path $root 'push'
Push-Location $push
try {
    if (-not (Test-Path 'node_modules')) { npm ci --no-fund --no-audit | Out-Host }
    Write-Host "`n== Push server tests"
    node --test test/server.test.js
    if ($LASTEXITCODE -ne 0) { throw 'push server tests failed' }
} finally {
    Pop-Location
}

Push-Location (Join-Path $root 'cloud')
try {
    if (-not (Test-Path 'node_modules')) { npm ci --no-fund --no-audit | Out-Host }
    Write-Host "`n== Cloud relay tests"
    npm test --silent
    if ($LASTEXITCODE -ne 0) { throw 'cloud tests failed' }
    Write-Host "`n== Cloud relay end-to-end (local Workers runtime)"
    node test/e2e-local.mjs
    if ($LASTEXITCODE -ne 0) { throw 'cloud e2e failed' }
} finally {
    Pop-Location
}
