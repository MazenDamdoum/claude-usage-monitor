# Shared helpers for the cloud scripts. Dot-source: . (Join-Path $PSScriptRoot 'common.ps1')

$AppDataDir = Join-Path $env:APPDATA 'ClaudeUsageMonitor'
$CloudConfig = Join-Path $AppDataDir 'cloud.json'           # read by the Windows app
$CloudSecrets = Join-Path $AppDataDir 'cloud-secrets.json'  # VAPID keys + ingest token (reused on redeploy)

function Invoke-Wrangler {
    $node = (Get-Command node -ErrorAction Stop).Source
    # Wrangler prints warnings (e.g. "update available") on stderr; that is not a failure.
    # Callers check $LASTEXITCODE instead.
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $env:WRANGLER_SEND_METRICS = 'false'
        & $node (Join-Path $PSScriptRoot 'node_modules\wrangler\bin\wrangler.js') @args 2>&1 | ForEach-Object {
            if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message -replace '^System\.Management\.Automation\.RemoteException$', '' } else { "$_" }
        }
    } finally {
        $ErrorActionPreference = $saved
    }
}

function Get-CloudConfig {
    if (-not (Test-Path $CloudConfig)) { throw 'The cloud relay is not set up yet. Run cloud\setup.ps1 first.' }
    return Get-Content $CloudConfig -Raw | ConvertFrom-Json
}

function Invoke-Cloud([string]$Method, [string]$Path, [string]$Body) {
    $cfg = Get-CloudConfig
    $params = @{ Method = $Method; Uri = "$($cfg.url)$Path"; Headers = @{ Authorization = "Bearer $($cfg.token)" }; TimeoutSec = 30 }
    if ($Body) { $params.ContentType = 'application/json'; $params.Body = $Body }
    return Invoke-RestMethod @params
}
