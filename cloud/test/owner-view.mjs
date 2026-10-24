// Screenshots a friend's phone page (Bob's), the way their phone shows it, from a local
// `wrangler dev` with made-up accounts. Two throwaway files in public/ sign the browser in as
// Bob; they are removed when this ends. Usage: node test/owner-view.mjs <out-dir>
import './setup.js';
import { execFile, spawn } from 'node:child_process';
import nodeCrypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { generateVapidKeys } from '../src/webpush.js';

const cloudDir = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const outDir = process.argv[2] || os.tmpdir();
const PORT = 8798;
const base = `http://127.0.0.1:${PORT}`;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const keys = await generateVapidKeys();
const ingestToken = nodeCrypto.randomBytes(32).toString('base64url');
const devVars = path.join(cloudDir, '.dev.vars');
fs.writeFileSync(devVars, `VAPID_PUBLIC_KEY=${keys.publicKey}\nVAPID_PRIVATE_KEY=${keys.privateKey}\nINGEST_TOKEN=${ingestToken}\n`);
const persist = fs.mkdtempSync(path.join(os.tmpdir(), 'cum-owner-view-'));
const loginHtml = path.join(cloudDir, 'public', '__login.html');
const loginJs = path.join(cloudDir, 'public', '__login.js');
const wrangler = spawn(process.execPath, [path.join(cloudDir, 'node_modules', 'wrangler', 'bin', 'wrangler.js'), 'dev', '--port', String(PORT), '--persist-to', persist], {
  cwd: cloudDir, env: { ...process.env, WRANGLER_SEND_METRICS: 'false' }, stdio: ['ignore', 'pipe', 'pipe'],
});
const ingest = (p, body) => fetch(`${base}${p}`, {
  method: body === undefined ? 'GET' : 'POST',
  headers: { Authorization: `Bearer ${ingestToken}`, 'Content-Type': 'application/json' },
  body: body === undefined ? undefined : JSON.stringify(body),
});
const now = Math.floor(Date.now() / 1000);
const acct = (email, name, active, five, week, fiveReset, weekReset) => ({
  email, name, active,
  // learnt costs like alice's: a full 5-hour session is about 15% of the week
  prompt_cost: { five_hour: { low: 41, medium: 51, high: 145 }, seven_day: { low: 6, medium: 8, high: 22 } },
  five_hour: { utilization_x100: five * 100, resets_at: fiveReset },
  seven_day: { utilization_x100: week * 100, resets_at: weekReset },
});
const series = Array.from({ length: 300 }, (_, i) => (i > 240 ? 120 + (i % 7) * 9 : i % 50 < 12 ? 80 : 0));
const upload = (bobActive) => ingest('/ingest/accounts', {
  user_name: 'Sam',
  usage_check: { at: now - 40, next: now + 102 },  // the header's countdown
  extras: { email: 'alice@example.com', items: [{ key: 'harbor_lantern', ends_at: now + 9 * 86400 + 4 * 3600 }] },  // a limit reset, as it might appear
  accounts: [acct('alice@example.com', 'alice', !bobActive, 40, 30, now + 7200, now + 400000),
             acct('bob@example.com', 'bob', bobActive, 57, 64, now + 3 * 3600 + 900, now + 300000)],
  tokens: { rate: 150, count: 0, working: true, pile: 1, mode: 'speed', series, history: [] },
});
const chrome = `${process.env.ProgramFiles}\\Google\\Chrome\\Application\\chrome.exe`;
const shot = (name) => new Promise((resolve) => execFile(chrome, [
  '--headless=new', '--disable-gpu', `--user-data-dir=${path.join(persist, 'chrome')}`, '--hide-scrollbars',
  '--force-device-scale-factor=2', '--window-size=520,2300', '--virtual-time-budget=6000',
  `--screenshot=${path.join(outDir, name)}`, `${base}/__login.html`,
], () => resolve()));

try {
  for (let i = 0; ; i++) {
    try { if ((await ingest('/ingest/health')).status === 200) break; } catch {}
    if (i > 120) throw new Error('wrangler dev did not start');
    await sleep(500);
  }
  await upload(true);
  const { code } = await (await ingest('/ingest/pair', { owner: 'bob' })).json();
  const { token } = await (await fetch(`${base}/api/pair`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ code }) })).json();
  await fetch(`${base}/api/owner/policy`, { method: 'PUT', headers: { Authorization: `Bearer ${token}`, 'Content-Type': 'application/json' }, body: JSON.stringify({ cap_five_hour: 70, cap_weekly: 80, allow: true }) });
  await upload(true);
  await ingest('/ingest/tokens', { rate: 150, count: 0, working: true, pile: 1, mode: 'speed', series, history: [] });
  fs.writeFileSync(loginJs, `localStorage.setItem('claude-usage.device-token', ${JSON.stringify(token)}); location.replace('/');\n`);
  fs.writeFileSync(loginHtml, '<!doctype html><script src="/__login.js"></script>\n');
  await shot('bob-in-use.png');
  await upload(false);  // Sam moves to alice
  await shot('bob-not-in-use.png');
  // The user's own page, for comparison.
  const pair2 = await (await ingest('/ingest/pair', {})).json();
  const me = await (await fetch(`${base}/api/pair`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ code: pair2.code }) })).json();
  await ingest('/ingest/tokens', { rate: 150, count: 0, working: true, pile: 1, mode: 'speed', series, history: [] });
  fs.writeFileSync(loginJs, `localStorage.setItem('claude-usage.device-token', ${JSON.stringify(me.token)}); location.replace('/');
`);
  await shot('user.png');
  console.log('ok');
} finally {
  for (const f of [loginHtml, loginJs, devVars]) fs.rmSync(f, { force: true });
  wrangler.kill();
}
