<#
.SYNOPSIS
  Deploys the cloud relay to your Cloudflare account (free plan) and connects
  the Windows app to it. Safe to run again: keys are reused, so paired phones
  keep working.
#>
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Set-Location $PSScriptRoot

if (-not (Get-Command node -ErrorAction SilentlyContinue)) { throw 'Node.js 18 or newer is required: https://nodejs.org' }
if (-not (Test-Path 'node_modules')) {
    Write-Host 'Installing dependencies...'
    npm ci --no-fund --no-audit | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'npm ci failed' }
}

# 1. Keys: generated once, kept outside the repository.
New-Item -ItemType Directory -Force $AppDataDir | Out-Null
if (-not (Test-Path $CloudSecrets)) {
    Write-Host 'Generating push keys and upload token...'
    $json = node --input-type=module -e @'
import './test/setup.js';
import { generateVapidKeys } from './src/webpush.js';
import { randomBytes } from 'node:crypto';
const k = await generateVapidKeys();
console.log(JSON.stringify({ VAPID_PUBLIC_KEY: k.publicKey, VAPID_PRIVATE_KEY: k.privateKey, INGEST_TOKEN: randomBytes(32).toString('base64url') }));
'@
    if ($LASTEXITCODE -ne 0) { throw 'key generation failed' }
    [IO.File]::WriteAllText($CloudSecrets, $json)
}
$secrets = Get-Content $CloudSecrets -Raw | ConvertFrom-Json

# 2. Cloudflare login (opens the browser the first time).
$who = Invoke-Wrangler whoami | Out-String
if ($who -match 'not authenticated') {
    Write-Host 'Sign in to Cloudflare in the browser window that opens...'
    Invoke-Wrangler login
    if ($LASTEXITCODE -ne 0) { throw 'wrangler login failed' }
}

# 3. Deploy the Worker + Durable Object.
Write-Host 'Deploying...'
$out = Invoke-Wrangler deploy | Out-String
Write-Host $out
if ($LASTEXITCODE -ne 0) { throw 'wrangler deploy failed (see above). New accounts may need a workers.dev subdomain: open the Workers page in the Cloudflare dashboard once.' }
$m = [regex]::Match($out, 'https://[a-z0-9.-]+\.workers\.dev')
if (-not $m.Success) { throw 'Could not find the workers.dev URL in the deploy output.' }
$url = $m.Value

# 4. Secrets (never stored in the repo or wrangler.toml).
$tmp = [IO.Path]::GetTempFileName()
try {
    [IO.File]::WriteAllText($tmp, ($secrets | ConvertTo-Json -Compress))
    Invoke-Wrangler secret bulk $tmp | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'setting secrets failed' }
} finally {
    Remove-Item $tmp -Force
}

# 5. Point the Windows app at it.
[IO.File]::WriteAllText($CloudConfig, (@{ url = $url; token = $secrets.INGEST_TOKEN } | ConvertTo-Json -Compress))

for ($i = 0; $i -lt 20; $i++) {
    try { $h = Invoke-Cloud GET '/ingest/health'; if ($h.ok) { break } } catch { Start-Sleep -Seconds 2 }
}
Write-Host ''
Write-Host "Cloud relay is live: $url" -ForegroundColor Green
Write-Host 'Next: run .\pair.ps1 and open the URL on your iPhone.'
Write-Host 'In the Windows app: Options > General > Phone Notifications must be ticked.'
