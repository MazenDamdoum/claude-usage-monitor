<#
.SYNOPSIS
  Builds ClaudeUsageMonitor.exe (the taskbar app) and claude-usage.exe (CLI probe).

.DESCRIPTION
  Needs no Visual Studio: downloads a pinned, checksum-verified portable Zig
  toolchain into .tools\ on first run and uses its bundled clang (MinGW target).
  If you prefer MSVC, use CMakeLists.txt instead.
#>
param(
    [switch]$Debug
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$root = Split-Path -Parent $PSScriptRoot
$zigVersion = '0.17.0'
$zigSha256 = 'b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428'
$zigName = "zig-x86_64-windows-$zigVersion"
$tools = Join-Path $root '.tools'
$zig = Join-Path $tools "$zigName\zig.exe"

if (-not (Test-Path $zig)) {
    New-Item -ItemType Directory -Force $tools | Out-Null
    $zip = Join-Path $tools "$zigName.zip"
    Write-Host "Downloading Zig $zigVersion toolchain..."
    Invoke-WebRequest "https://ziglang.org/download/$zigVersion/$zigName.zip" -OutFile $zip -UseBasicParsing
    $actual = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    if ($actual -ne $zigSha256) {
        Remove-Item $zip -Force
        throw "Zig download checksum mismatch (got $actual)"
    }
    tar -xf $zip -C $tools
    if ($LASTEXITCODE -ne 0) { throw 'Failed to extract Zig' }
    Remove-Item $zip -Force
}

$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null
$src = Join-Path $root 'src'
$res = Join-Path $root 'res'

$flags = @('-target', 'x86_64-windows-gnu', '-std=c++17', '-Wall', '-Wextra', '-Wno-unused-parameter',
           '-DUNICODE', '-D_UNICODE', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX')
if ($Debug) { $flags += @('-O0', '-g') } else { $flags += @('-O2', '-s') }

Write-Host 'Building ClaudeUsageMonitor.exe...'
$appSources = 'main.cpp', 'TaskbarWidget.cpp', 'MainWindow.cpp', 'OptionsDialog.cpp',
    'Canvas.cpp', 'UsageView.cpp', 'UsagePoller.cpp', 'UsageClient.cpp', 'UsageIcon.cpp', 'Settings.cpp' |
    ForEach-Object { Join-Path $src $_ }
& $zig c++ @flags '-Wl,--subsystem,windows' -o (Join-Path $out 'ClaudeUsageMonitor.exe') @appSources (Join-Path $res 'app.rc') `
    -lwinhttp -lshell32 -luser32 -lgdi32 -lcomctl32 -lcomdlg32 -ladvapi32 -lkernel32
if ($LASTEXITCODE -ne 0) { throw 'App build failed' }

Write-Host 'Building claude-usage.exe...'
& $zig c++ @flags -o (Join-Path $out 'claude-usage.exe') `
    (Join-Path $src 'cli.cpp') (Join-Path $src 'UsageClient.cpp') `
    -lwinhttp -lkernel32
if ($LASTEXITCODE -ne 0) { throw 'CLI build failed' }

Get-ChildItem $out -Include *.lib, *.pdb -Recurse -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
Write-Host "Done -> $out"
Get-ChildItem $out -File -Filter *.exe | Format-Table Name, Length -AutoSize
