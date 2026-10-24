// End-to-end in the real Workers runtime (wrangler dev / workerd), no Cloudflare
// account needed:
//   phone pairs + subscribes  →  C++ bridge uploads usage (5h reset in a few s)
//   →  Durable Object alarm fires  →  encrypted push reaches a fake push service.
//
// Run:  node test/e2e-local.mjs        (needs ..\build\push_bridge_test.exe)

import './setup.js';
import assert from 'node:assert/strict';
import { execFile, spawn } from 'node:child_process';
import nodeCrypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { generateVapidKeys } from '../src/webpush.js';

const ece = createRequire(import.meta.url)('http_ece');
const cloudDir = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const exe = path.join(cloudDir, '..', 'build', 'push_bridge_test.exe');
const PORT = 8799;
const base = `http://127.0.0.1:${PORT}`;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const received = [];
const fake = http.createServer((req, res) => {
  const chunks = [];
  req.on('data', (c) => chunks.push(c));
  req.on('end', () => {
    received.push({ path: req.url, headers: req.headers, body: Buffer.concat(chunks) });
    res.statusCode = 201;
    res.end();
  });
});
await new Promise((r) => fake.listen(0, '127.0.0.1', r));
const pushOrigin = `http://127.0.0.1:${fake.address().port}`;

const keys = await generateVapidKeys();
const ingestToken = nodeCrypto.randomBytes(32).toString('base64url');
const devVars = path.join(cloudDir, '.dev.vars');
fs.writeFileSync(devVars, `VAPID_PUBLIC_KEY=${keys.publicKey}\nVAPID_PRIVATE_KEY=${keys.privateKey}\nINGEST_TOKEN=${ingestToken}\nTEST_PUSH_ORIGIN=${pushOrigin}\n`);
const persist = fs.mkdtempSync(path.join(os.tmpdir(), 'cum-cloud-e2e-'));
const cloudJson = path.join(persist, 'cloud.json');
fs.writeFileSync(cloudJson, JSON.stringify({ url: base, token: ingestToken }));

const wrangler = spawn(process.execPath, [path.join(cloudDir, 'node_modules', 'wrangler', 'bin', 'wrangler.js'), 'dev', '--port', String(PORT), '--persist-to', persist], {
  cwd: cloudDir,
  env: { ...process.env, WRANGLER_SEND_METRICS: 'false' },
  stdio: ['ignore', 'pipe', 'pipe'],
});
let logs = '';
wrangler.stdout.on('data', (d) => (logs += d));
wrangler.stderr.on('data', (d) => (logs += d));

const step = (msg) => console.log(`  ✓ ${msg}`);
const ingest = (p, body) =>
  fetch(`${base}${p}`, {
    method: body === undefined ? 'GET' : 'POST',
    headers: { Authorization: `Bearer ${ingestToken}`, 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });

try {
  for (let i = 0; ; i++) {
    try {
      if ((await ingest('/ingest/health')).status === 200) break;
    } catch {}
    if (i > 120) throw new Error(`wrangler dev did not start:\n${logs}`);
    await sleep(500);
  }
  step('wrangler dev (workerd) is serving the Worker + Durable Object');

  const page = await fetch(`${base}/`);
  assert.equal(page.status, 200);
  assert.match(await page.text(), /apple-touch-icon/);
  assert.match(page.headers.get('content-security-policy'), /default-src 'self'/);
  assert.equal((await fetch(`${base}/manifest.webmanifest`)).status, 200);
  step('phone app + manifest served with security headers');

  const { code } = await (await ingest('/ingest/pair', {})).json();
  const pair = await fetch(`${base}/api/pair`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ code }) });
  assert.equal(pair.status, 201);
  const { token } = await pair.json();
  const ecdh = nodeCrypto.createECDH('prime256v1');
  ecdh.generateKeys();
  const auth = nodeCrypto.randomBytes(16);
  const sub = { endpoint: `${pushOrigin}/phone`, keys: { p256dh: ecdh.getPublicKey().toString('base64url'), auth: auth.toString('base64url') } };
  const subRes = await fetch(`${base}/api/subscribe`, {
    method: 'POST',
    headers: { Authorization: `Bearer ${token}`, 'Content-Type': 'application/json' },
    body: JSON.stringify({ subscription: sub }),
  });
  assert.equal(subRes.status, 200);
  step('phone paired with a one-time code and subscribed');

  const run = await new Promise((resolve) =>
    execFile(exe, ['--cloud', 'usage', '6'], { env: { ...process.env, CUM_CLOUD_CONFIG_FILE: cloudJson } }, (err, stdout) =>
      resolve({ code: err ? err.code : 0, stdout: stdout.trim() })),
  );
  assert.equal(run.code, 0, `C++ bridge: ${run.stdout}`);
  const health = await (await ingest('/ingest/health')).json();
  assert.ok(health.schedule.five_hour_reset > 0);
  step(`C++ bridge uploaded usage over HTTP (status ${run.stdout}); 5-hour reset scheduled`);

  // (The upload at 42% also brings the 25% usage alert; the reset alert follows at the reset.)
  const notes = () => received.map((r) => ({ r, n: JSON.parse(ece.decrypt(r.body, { version: 'aes128gcm', privateKey: ecdh, authSecret: auth }).toString()) }));
  for (let i = 0; i < 60 && !notes().some(({ n }) => n.title === 'Claude: 5h limit reset'); i++) await sleep(500);
  const resets = notes().filter(({ n }) => n.title === 'Claude: 5h limit reset');
  assert.equal(resets.length, 1, 'alarm should deliver exactly one reset push');
  assert.ok(notes().some(({ n }) => n.title === 'Claude: 42% of 5h used'), 'and the 25% usage alert');
  assert.match(resets[0].r.headers.authorization, /^vapid t=/);
  step('Durable Object alarm fired at the reset time and sent an encrypted, VAPID-signed push');

  const dup = await new Promise((resolve) =>
    execFile(exe, ['--cloud', 'event'], { env: { ...process.env, CUM_CLOUD_CONFIG_FILE: cloudJson } }, (err, stdout) => resolve(stdout.trim())),
  );
  await sleep(1000);
  assert.ok(Number(dup) >= 200 && Number(dup) < 300);
  step('confirmed event from the C++ bridge accepted');

  const widgetKey = (await (await fetch(`${base}/api/widget-key`, { method: 'POST', headers: { Authorization: `Bearer ${token}` } })).json()).key;
  const widget = await (await fetch(`${base}/api/widget`, { headers: { Authorization: `Bearer ${widgetKey}` } })).json();
  assert.equal(widget.usage.five_hour.percent, 42);
  step('read-only widget key returns usage for Scriptable');

  console.log('\nE2E PASSED');
} catch (err) {
  console.error('\nE2E FAILED:', err.message);
  console.error(logs.split('\n').slice(-30).join('\n'));
  process.exitCode = 1;
} finally {
  wrangler.kill();
  fake.close();
  fs.rmSync(devVars, { force: true });
}
