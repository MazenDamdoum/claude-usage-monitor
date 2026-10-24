// The real Windows app, end to end, in a sandbox. Nothing real is touched: a copy of
// ClaudeUsageMonitor.exe runs with its own settings next to it, a fake usage API, a fake
// claude-swap with two fake accounts (nayef, huey), and the cloud relay in the local Workers
// runtime. Three fake phones (yours, huey's, nayef's) decrypt every notification they get.
//
// Checks: the app switches accounts before a limit is hit, from Claude Code's own transcripts
// while the usage API is rate-limited (so the work carries on); a "limit hit" from Claude Code's
// StopFailure hook switches at once; account switches notify you and both owners; usage
// alerts; an owner's cap and
// "stop" request reach your phone and make the app switch away (or explain why it can't);
// clicking a closed account is refused; every account's resets; the delayed test push.
//
// Run:  node test/app-e2e.mjs      (after scripts\build.ps1; takes a few minutes)

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
const builtExe = path.join(cloudDir, '..', 'build', 'ClaudeUsageMonitor.exe');
const PORT = 8797;
const base = `http://127.0.0.1:${PORT}`;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const nowS = () => Math.floor(Date.now() / 1000);
const iso = (s) => new Date(s * 1000).toISOString();
const step = (msg) => console.log(`  ✓ ${msg}`);

async function waitFor(what, check, seconds = 120) {
  for (let i = 0; i < seconds * 2; i++) {
    const v = await check();
    if (v) return v;
    await sleep(500);
  }
  throw new Error(`timed out waiting for: ${what}`);
}

// ---- sandbox: the app's folder, fake claude-swap state, fake login --------------------

const box = fs.mkdtempSync(path.join(os.tmpdir(), 'cum-app-e2e-'));
const exe = path.join(box, 'ClaudeUsageMonitor.exe');
fs.copyFileSync(builtExe, exe);
const stateFile = path.join(box, 'state.json');
const credsFile = path.join(box, '.credentials.json');
const logFile = path.join(box, 'app.log');

// Claude Code's transcripts (<config dir>\projects\*\*.jsonl): the app learns from them.
const transcript = path.join(box, 'projects', 'demo', 'session.jsonl');
fs.mkdirSync(path.dirname(transcript), { recursive: true });
fs.writeFileSync(transcript, JSON.stringify({ type: 'user', promptId: 'p1', message: { content: 'build it' } }) + '\n');
let replies = 0;
function spend(units) {  // one Opus reply worth `units` (output tokens weigh 5)
  fs.appendFileSync(transcript, JSON.stringify({
    type: 'assistant', effort: 'high', timestamp: new Date().toISOString(),
    message: { id: `msg_${++replies}`, model: 'claude-opus-5-5', content: [], usage: { input_tokens: 0, cache_creation_input_tokens: 0, cache_read_input_tokens: 0, output_tokens: units / 5 } },
  }) + '\n');
}
let apiLimited = false;  // the usage API answering 429, as it often does

const t0 = nowS();
const accounts = {
  1: { email: 'nayef@example.com', alias: 'nayef', token: 'tok-nayef', five: 20, week: 30, fiveReset: t0 + 4 * 3600, weekReset: t0 + 3 * 86400 },
  2: { email: 'huey@example.com', alias: 'huey', token: 'tok-huey', five: 10, week: 15, fiveReset: t0 + 2 * 3600, weekReset: t0 + 5 * 86400 },
};
// A window whose reset time has passed starts again empty, as the real usage API reports it.
function rolled(a) {
  const t = Math.floor(Date.now() / 1000);
  return a.fiveReset <= t ? { ...a, five: 0, fiveReset: t + 5 * 3600 } : a;
}
const readState = () => JSON.parse(fs.readFileSync(stateFile, 'utf8'));
function writeState(state) {
  fs.writeFileSync(stateFile, JSON.stringify(state));
  const a = state.accounts[state.active];
  fs.writeFileSync(credsFile, JSON.stringify({ claudeAiOauth: { accessToken: a.token, expiresAt: Date.now() + 86400_000, subscriptionType: 'max' } }));
}
function setUsage(number, changes) {
  const s = readState();
  Object.assign(s.accounts[number], changes);
  writeState(s);
}
writeState({ active: 1, accounts });

// claude-swap stand-in: `list --json` and `switch N --json` over state.json.
fs.writeFileSync(path.join(box, 'cswap.mjs'), `
import fs from 'node:fs';
const file = ${JSON.stringify(stateFile)};
const creds = ${JSON.stringify(credsFile)};
const s = JSON.parse(fs.readFileSync(file, 'utf8'));
const iso = (t) => new Date(t * 1000).toISOString();
const now = Math.floor(Date.now() / 1000);
for (const a of Object.values(s.accounts)) if (a.fiveReset <= now) { a.five = 0; a.fiveReset = now + 5 * 3600; }
const [cmd, arg] = process.argv.slice(2);
if (cmd === 'list') {
  console.log(JSON.stringify({ schemaVersion: 1, activeAccountNumber: s.active, accounts: Object.entries(s.accounts).map(([n, a]) => ({
    number: Number(n), email: a.email, alias: a.alias, active: Number(n) === s.active, usageStatus: 'ok', disabled: false,
    usage: { fiveHour: { pct: a.five, resetsAt: iso(a.fiveReset) }, sevenDay: { pct: a.week, resetsAt: iso(a.weekReset) }, scoped: [] },
  })) }));
} else if (cmd === 'switch') {
  const from = s.active;
  s.active = Number(arg);
  fs.writeFileSync(file, JSON.stringify(s));
  const a = s.accounts[s.active];
  fs.writeFileSync(creds, JSON.stringify({ claudeAiOauth: { accessToken: a.token, expiresAt: Date.now() + 86400000, subscriptionType: 'max' } }));
  console.log(JSON.stringify({ switched: true, from: { number: from, email: s.accounts[from].email }, to: { number: s.active, email: a.email } }));
} else if (cmd === 'import-usage') {
  process.stdin.resume();  // the app hands over a reading; nothing to do here
  process.stdin.on('end', () => process.exit(0));
} else {
  process.exit(2);
}
`);
fs.writeFileSync(path.join(box, 'cswap.cmd'), `@"${process.execPath}" "%~dp0cswap.mjs" %*\r\n`);

// ---- fake usage API (what api.anthropic.com answers for each login) --------------------

const api = http.createServer((req, res) => {
  const token = (req.headers.authorization || '').replace('Bearer ', '');
  const found = Object.values(readState().accounts).find((a) => a.token === token);
  const acc = found && rolled(found);
  res.setHeader('Content-Type', 'application/json');
  if (!acc) { res.statusCode = 401; return res.end('{}'); }
  if (apiLimited && req.url === '/api/oauth/usage') { res.statusCode = 429; return res.end('{}'); }
  if (req.url === '/api/oauth/profile') return res.end(JSON.stringify({ account: { email: acc.email } }));
  if (req.url === '/api/oauth/usage') {
    return res.end(JSON.stringify({
      five_hour: { utilization: acc.five, resets_at: iso(acc.fiveReset) },
      seven_day: { utilization: acc.week, resets_at: iso(acc.weekReset) },
    }));
  }
  res.statusCode = 404;
  res.end('{}');
});
await new Promise((r) => api.listen(0, '127.0.0.1', r));

// ---- fake push service + phones ----------------------------------------------------------

const received = [];
const pushServer = http.createServer((req, res) => {
  const chunks = [];
  req.on('data', (c) => chunks.push(c));
  req.on('end', () => {
    received.push({ path: req.url, body: Buffer.concat(chunks), at: Date.now() });
    res.statusCode = 201;
    res.end();
  });
});
await new Promise((r) => pushServer.listen(0, '127.0.0.1', r));
const pushOrigin = `http://127.0.0.1:${pushServer.address().port}`;

const phones = {};
function inbox(name) {
  const p = phones[name];
  return received.filter((r) => r.path === `/${name}`)
    .map((r) => JSON.parse(ece.decrypt(r.body, { version: 'aes128gcm', privateKey: p.ecdh, authSecret: p.auth }).toString()));
}
const has = (name, re) => inbox(name).find((n) => re.test(`${n.title} | ${n.body}`));

// ---- cloud relay in workerd ----------------------------------------------------------

const keys = await generateVapidKeys();
const ingestToken = nodeCrypto.randomBytes(32).toString('base64url');
const devVars = path.join(cloudDir, '.dev.vars');
fs.writeFileSync(devVars, `VAPID_PUBLIC_KEY=${keys.publicKey}\nVAPID_PRIVATE_KEY=${keys.privateKey}\nINGEST_TOKEN=${ingestToken}\nTEST_PUSH_ORIGIN=${pushOrigin}\n`);
const persist = path.join(box, 'cloud-state');
const cloudJson = path.join(box, 'cloud.json');
fs.writeFileSync(cloudJson, JSON.stringify({ url: base, token: ingestToken }));
const wrangler = spawn(process.execPath, [path.join(cloudDir, 'node_modules', 'wrangler', 'bin', 'wrangler.js'), 'dev', '--port', String(PORT), '--persist-to', persist], {
  cwd: cloudDir, env: { ...process.env, WRANGLER_SEND_METRICS: 'false' }, stdio: ['ignore', 'pipe', 'pipe'],
});
let logs = '';
wrangler.stdout.on('data', (d) => (logs += d));
wrangler.stderr.on('data', (d) => (logs += d));

const ingest = (p, body) => fetch(`${base}${p}`, {
  method: body === undefined ? 'GET' : 'POST',
  headers: { Authorization: `Bearer ${ingestToken}`, 'Content-Type': 'application/json' },
  body: body === undefined ? undefined : JSON.stringify(body),
});
const phoneApi = (name, method, p, body) => fetch(`${base}/api${p}`, {
  method,
  headers: { Authorization: `Bearer ${phones[name].token}`, 'Content-Type': 'application/json' },
  body: body === undefined ? undefined : JSON.stringify(body),
});

async function pairPhone(name, owner) {
  const res = await ingest('/ingest/pair', owner ? { owner } : {});
  if (res.status !== 200) return false;
  const { code } = await res.json();
  const { token } = await (await fetch(`${base}/api/pair`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ code }) })).json();
  const ecdh = nodeCrypto.createECDH('prime256v1');
  ecdh.generateKeys();
  const auth = nodeCrypto.randomBytes(16);
  phones[name] = { token, ecdh, auth };
  const sub = { endpoint: `${pushOrigin}/${name}`, keys: { p256dh: ecdh.getPublicKey().toString('base64url'), auth: auth.toString('base64url') } };
  assert.equal((await phoneApi(name, 'POST', '/subscribe', { subscription: sub })).status, 200);
  return true;
}

// Left-click on a taskbar column of the sandboxed app (the click handler, without the mouse).
// Posts a message to the sandboxed app's window (by its process id), or closes its dialogs.
function toApp(pid, message, wparam, dialogs = false) {
  const ps = `
Add-Type -Namespace E2E -Name W -MemberDefinition @'
[DllImport("user32.dll")] public static extern System.IntPtr FindWindowExW(System.IntPtr p, System.IntPtr a, [MarshalAs(UnmanagedType.LPWStr)] string c, System.IntPtr w);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool PostMessageW(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);
'@
$h = [System.IntPtr]::Zero
$found = $false
while ($true) {
  $h = [E2E.W]::FindWindowExW([System.IntPtr]::Zero, $h, '${dialogs ? '#32770' : 'ClaudeUsageMonitor.App'}', [System.IntPtr]::Zero)
  if ($h -eq [System.IntPtr]::Zero) { if ($found) { exit 0 } else { exit 1 } }
  $p = 0; [void][E2E.W]::GetWindowThreadProcessId($h, [ref]$p)
  if ($p -eq ${pid}) { [void][E2E.W]::PostMessageW($h, ${message}, [System.IntPtr]${wparam}, [System.IntPtr]::Zero); $found = $true; if (-not ${dialogs ? '$true' : '$false'}) { exit 0 } }
}`;
  return new Promise((resolve) => execFile('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', ps], (err) => resolve(!err)));
}

const clickColumn = (pid, column) => toApp(pid, 0x8016, column);   // TaskbarWidget::kMsgColumnClick
const menuCommand = (pid, command) => toApp(pid, 0x8003, command);  // kMsgRemote: as if picked in the menu
const closeDialogs = (pid) => toApp(pid, 0x0010, 0, true);         // WM_CLOSE

const appLog = () => (fs.existsSync(logFile) ? fs.readFileSync(logFile, 'utf8') : '');
const active = () => readState().active;

// ---- the app ---------------------------------------------------------------------------

fs.writeFileSync(path.join(box, 'settings.ini'), [
  '[general]',
  `credentials_path=${credsFile}`,
  'phone_notifications=1', 'accounts=1', `cswap_path=${path.join(box, 'cswap.cmd')}`,
  'auto_switch=1', 'auto_switch_percent=95', 'your_name=Mazen', 'show_tray_icon=0', 'notify=0',
  '[taskbar]', 'visible=0', '[main_window]', 'visible=0', '',
].join('\r\n'));
let app = null;

try {
  await waitFor('wrangler dev', async () => { try { return (await ingest('/ingest/health')).status === 200; } catch { return false; } }, 90);
  step('local cloud relay is up');

  app = spawn(exe, [], {
    env: {
      ...process.env,
      CUM_INSTANCE: '.e2e',
      CUM_TEST_API: `http://127.0.0.1:${api.address().port}`,
      CUM_TEST_POLL_SECONDS: '5',
      CUM_CLOUD_CONFIG_FILE: cloudJson,
      CUM_PUSH_BRIDGE_FILE: path.join(box, 'no-local-push-server.json'),
    },
    stdio: 'ignore',
  });
  await waitFor('the app to upload both accounts', () => pairPhone('huey', 'huey'), 90);
  assert.ok(await pairPhone('me'));
  assert.ok(await pairPhone('nayef', 'nayef'));
  step('app uploads both accounts; your phone, huey\'s and nayef\'s are paired');

  const mine = await (await phoneApi('me', 'GET', '/status')).json();
  assert.deepEqual(mine.accounts.map((a) => [a.name, a.active]), [['nayef', true], ['huey', false]]);
  const hueys = await (await phoneApi('huey', 'GET', '/status')).json();
  assert.equal(hueys.account.name, 'huey');
  assert.equal(hueys.accounts, undefined);
  assert.ok(!JSON.stringify(hueys).includes('nayef'), "huey's phone never sees nayef's account");
  step('your phone sees both accounts; huey\'s phone sees only huey');

  // 0. Claude Code works on nayef and its usage moves with it: the app learns the rate.
  for (let i = 1; i <= 4; i++) {
    spend(100_000);
    await sleep(1500);
    setUsage(1, { five: 20 + i, week: 30 + i });
    await sleep(7000);
  }
  await waitFor('the app to learn the rate', () => {
    const file = path.join(box, 'calibration.txt');
    const m = fs.existsSync(file) && /nayef@example\.com 0 ([\d.]+) /.exec(fs.readFileSync(file, 'utf8'));
    return m && Number(m[1]) >= 3;
  }, 90);
  step('the app learnt from the transcripts how fast nayef\'s limit fills (≈1% per 100k units)');

  // 1. nayef nears the limit while the usage API is rate-limited: the app sees the work in
  //    the transcripts and switches before the limit, so Claude Code never stops.
  setUsage(1, { five: 90 });  // a jump no Claude Code work explains: must not skew the learnt rate
  await waitFor('the app to read 90%', async () => {
    const st = await (await phoneApi('me', 'GET', '/status')).json();
    return st.accounts.find((a) => a.name === 'nayef').five_hour.percent === 90;
  }, 60);
  apiLimited = true;
  spend(700_000);  // ≈ +7% at the learnt rate: 97%, but no usage check gets through
  await waitFor('switch to huey on the live estimate', () => active() === 2, 60);
  apiLimited = false;
  setUsage(1, { five: 97 });  // what the API would show by now
  await waitFor('your switch notification', () => has('me', /Switched to huey \| nayef hit 9[5-9]% \(5h\)\. Claude keeps going\./), 60);
  await waitFor("huey's 'started using' alert", () => has('huey', /Mazen is now on your account/), 90);
  await waitFor("nayef's 'switched away' alert", () => has('nayef', /Mazen left your account/), 60);
  assert.ok(has('nayef', /nayef: 90% of 5h used \| 10% left · resets in .+ · Mazen on it/), "nayef's usage alert");
  assert.ok(has('huey', /Mazen may switch to you soon \| In about 15–30 min\. Tap for your usage\./), "huey's heads-up at 15% left");
  assert.ok(has('me', /nayef: 90% of 5h used \| 10% left · resets in .+ · in use/), 'your usage alert');
  assert.match(appLog(), /notify: Switched to huey \| nayef hit 9[5-9]% \(5h\)\. Claude keeps going\. \| \+ phone/);
  step('nayef at 90% + 7% of work the usage API never reported: switched to huey before the limit; you, huey and nayef notified');

  // 2. Live usage alerts for the owner. Meanwhile nayef's 5-hour window resets.
  setUsage(1, { five: 10, fiveReset: nowS() + 5 * 3600 });
  setUsage(2, { five: 55 });
  await waitFor("huey's 50% alert", () => has('huey', /huey: 55% of 5h used \| 45% left · resets in/), 90);
  step('huey was told "55% used · 45% left" as you used his account');

  // 3. huey caps his account at 50% from his phone: you're told, and the app moves you off it.
  await waitFor("the app to see nayef's reset (claude-swap's view)", async () => {
    const s = await (await phoneApi('me', 'GET', '/status')).json();
    return s.accounts.find((a) => a.name === 'nayef').five_hour.percent === 10;
  }, 120);
  assert.equal((await phoneApi('huey', 'PUT', '/owner/policy', { cap_five_hour: 50, cap_weekly: null, allow: true })).status, 200);
  await waitFor('your cap notification', () => has('me', /huey updated access \| Capped at 5h 50%\./), 30);
  await waitFor('app switches off the capped account', () => active() === 1, 120);
  await waitFor('your switch notification', () => has('me', /Switched to nayef \| huey hit the 50% 5h cap\. Claude keeps going\./), 60);
  await waitFor("huey's 'switched away' alert", () => has('huey', /Mazen left your account/), 90);
  assert.match(appLog(), /owners: huey updated access: Capped at 5h 50%\./);
  step('huey capped at 50%: your phone and PC were told, and the app switched you back to nayef');

  // 4. You can still pick huey by hand while he's over his cap: the app switches and stays
  //    (caps steer the automatic switching, not your picks); clicking nayef brings you back.
  // The taskbar puts the account in use first (column 0 is the token speed), the rest in order.
  const columnOf = async (name) => {
    const list = (await (await phoneApi('me', 'GET', '/status')).json()).accounts;
    const shown = [...list.filter((a) => a.active), ...list.filter((a) => !a.active)];
    return shown.findIndex((a) => a.name === name) + 1;
  };
  assert.ok(await clickColumn(app.pid, await columnOf('huey')), 'posted the click');
  await waitFor('switch to capped huey by hand', () => active() === 2, 20);
  await waitFor('switch confirmed', () => /accounts: switch to huey@example\.com confirmed/.test(appLog()), 30);
  await sleep(12000);
  assert.equal(active(), 2, 'stays on huey: you chose it');
  assert.ok(await clickColumn(app.pid, await columnOf('nayef')), 'posted the click');
  await waitFor('back to nayef', () => active() === 1, 20);
  step('picked capped huey by hand: switched, confirmed and stayed; clicked nayef: back');

  // 5. nayef asks you to stop while huey is still capped: nowhere to go, and you're told why.
  assert.equal((await phoneApi('nayef', 'PUT', '/owner/policy', { cap_five_hour: null, cap_weekly: null, allow: false })).status, 200);
  await waitFor('your stop notification', () => has('me', /nayef updated access \| Asked you to stop using it\./), 30);
  await waitFor('no-room notice', () => has('me', /No Claude account has room \| nayef asked you to stop\. No other account has room\./), 120);
  assert.equal(active(), 1);
  step('nayef asked you to stop while huey was capped: you were told no other account has room');

  // 6. huey lifts his cap: the app honours nayef's request at once.
  assert.equal((await phoneApi('huey', 'PUT', '/owner/policy', { cap_five_hour: null, cap_weekly: null, allow: true })).status, 200);
  await waitFor('your notification', () => has('me', /huey updated access \| Caps removed\./), 30);
  await waitFor('switch to huey', () => active() === 2, 120);
  await waitFor('your switch notification', () => has('me', /Switched to huey \| nayef asked you to stop\. Claude keeps going\./), 60);
  step("huey lifted his cap: the app moved you off nayef's account as nayef asked");

  // 7. Every account's resets: you hear about both, each owner only about his own.
  const soon = nowS() + 40;
  setUsage(2, { fiveReset: soon });
  await waitFor('your reset alert', () => has('me', /huey: 5h limit reset \| Full usage available\./), 120);
  await waitFor("huey's reset alert", () => has('huey', /huey: 5h limit reset/), 30);
  assert.ok(!has('nayef', /huey/), "nayef never hears about huey's account");
  step("huey's 5-hour reset: you and huey were notified, nayef wasn't");

  // 8. Safety net: Claude Code stopped on a limit (its StopFailure hook runs `--limit-hit`):
  //    the app switches at once, even with huey far from his limit by its own numbers.
  assert.equal((await phoneApi('nayef', 'PUT', '/owner/policy', { cap_five_hour: null, cap_weekly: null, allow: true })).status, 200);
  setUsage(1, { five: 10, fiveReset: nowS() + 5 * 3600 });
  await waitFor("the app to see nayef's room again", async () => {
    const st = await (await phoneApi('me', 'GET', '/status')).json();
    return st.accounts.find((a) => a.name === 'nayef').five_hour.percent === 10;
  }, 120);
  await waitFor('nayef allows again (PC)', () => /owners: nayef updated access: You can use it again\./.test(appLog()), 90);
  const hook = await new Promise((resolve) => execFile(exe, ['--limit-hit'], { env: { ...process.env, CUM_INSTANCE: '.e2e' } }, (err) => resolve(!err)));
  assert.ok(hook, 'hook command ran');
  await waitFor('switch to nayef', () => active() === 1, 30);
  await waitFor('your switch notification', () => has('me', /Switched to nayef \| huey hit its 5h limit\. Claude keeps going\./), 60);
  await waitFor("nayef's 'started using' alert", () => inbox('nayef').filter((n) => /Mazen is now on/.test(n.title)).length >= 2, 90);
  step('Claude Code\'s limit hook: switched huey → nayef at once; you and nayef notified');

  // 9. Your own cap (agreed with nayef in person): the lower of yours and his applies.
  assert.equal((await phoneApi('me', 'PUT', '/my-cap', { account: 'nayef', cap_five_hour: 10, cap_weekly: null })).status, 200);
  await waitFor('switch off nayef', () => active() === 2, 120);
  await waitFor('your switch notification', () => has('me', /Switched to huey \| nayef hit the 10% 5h cap\. Claude keeps going\./), 60);
  assert.match(appLog(), /owners: Your cap on nayef: Set at 5h 10%\./);
  step('your own 10% cap on nayef (he set none): the app moved you to huey');

  // 10. Nothing has room, then the other account resets: switch right then, not 5 minutes later,
  //     and "no room" is said once even as the percentage creeps up.
  assert.equal((await phoneApi('me', 'PUT', '/my-cap', { account: 'nayef', cap_five_hour: null, cap_weekly: null })).status, 200);
  const nayefReset = nowS() + 90;  // time for claude-swap's once-a-minute check to see 97%
  setUsage(1, { five: 97, fiveReset: nayefReset });
  await waitFor("the app to see nayef at 97%", async () => {
    const st = await (await phoneApi('me', 'GET', '/status')).json();
    return st.accounts.find((a) => a.name === 'nayef').five_hour.percent === 97;
  }, 120);
  const noRoom = inbox('me').filter((n) => /No Claude account has room/.test(n.title)).length;
  setUsage(2, { five: 96 });
  await waitFor('no-room notice', () => inbox('me').filter((n) => /No Claude account has room/.test(n.title)).length > noRoom, 60);
  setUsage(2, { five: 97 });
  await waitFor('switch to nayef right after its reset', () => active() === 1, Math.max(5, nayefReset - nowS()) + 45);
  assert.ok(nowS() - nayefReset < 45, `switched ${nowS() - nayefReset}s after nayef's reset`);
  assert.equal(inbox('me').filter((n) => /No Claude account has room/.test(n.title)).length, noRoom + 1, 'said once');
  step(`nothing had room; nayef reset and the app switched ${nowS() - nayefReset}s later, with one "no room" notice`);

  // 11. Switching by hand: the taskbar shows it at once and you're told.
  const fromCol = (await (await phoneApi('me', 'GET', '/status')).json()).accounts.findIndex((a) => a.name === 'huey') + 1;
  const clickedAt = Date.now();
  const picked = () => inbox('me').filter((n) => /^Switched to huey$/.test(n.title) && /You picked it/.test(n.body)).length;
  const pickedBefore = picked();
  assert.ok(await clickColumn(app.pid, fromCol));
  await waitFor('your manual-switch notification', () => picked() > pickedBefore, 30);
  await waitFor('switched to huey', () => active() === 2, 10);
  step(`clicked huey: switched and you were told within ${((Date.now() - clickedAt) / 1000).toFixed(0)} s`);

  // 12. Right-click > Phone link > For huey's owner: a fresh one-time link.
  assert.ok(await menuCommand(app.pid, 500 + 2));
  await waitFor("a new link for huey's owner", async () => (await (await ingest('/ingest/health')).json()).links.some((l) => l.for === 'huey'), 30);
  await sleep(500);
  await closeDialogs(app.pid);
  step("right-click > Phone link made a one-time link for huey's owner");

  // 13. Switching from the phone: the PC picks the request up within seconds and you're told.
  const askedAt = Date.now();
  assert.equal((await phoneApi('me', 'POST', '/switch', { account: 'nayef' })).status, 200);
  await waitFor('the PC to switch to nayef', () => active() === 1, 30);
  await waitFor('your notification', () => has('me', /^Switched to nayef \| From your phone\. Claude keeps going\.$/), 30);
  step(`switched to nayef from the phone in ${((Date.now() - askedAt) / 1000).toFixed(0)} s`);

  // 14. The test notification arrives 10 seconds after the tap.
  const before = inbox('me').length;
  const tapped = Date.now();
  assert.equal((await phoneApi('me', 'POST', '/test')).status, 200);
  await sleep(4000);
  assert.equal(inbox('me').length, before, 'not yet');
  await waitFor('test notification', () => has('me', /^Claude Usage test \| Now: /), 30);
  const sample = has('me', /^Claude Usage test/).body;
  for (const [n, a] of Object.entries(readState().accounts)) {
    assert.ok(sample.includes(`${a.alias} 5h ${a.five}%`), `test shows ${a.alias}'s real 5h usage (${sample})`);
  }
  const delay = (received.filter((r) => r.path === '/me').at(-1).at - tapped) / 1000;
  assert.ok(delay >= 9, `arrived after ${delay}s`);
  step(`test notification arrived ${delay.toFixed(0)} s after the tap`);

  console.log('\nAPP E2E PASSED');
  console.log('\nCalibration samples:\n' + appLog().split('\n').filter((l) => /calibration:|auto-switch:/.test(l)).join('\n'));
  console.log('\nWhat each phone received:');
  for (const name of ['me', 'huey', 'nayef']) {
    console.log(`  ${name}:`);
    for (const n of inbox(name)) console.log(`    - ${n.title} | ${n.body}`);
  }
} catch (err) {
  console.error('\nAPP E2E FAILED:', err.message);
  console.error('--- calibration ---\n' + appLog().split('\n').filter((l) => /calibration:|auto-switch|ok  5h=9|error/.test(l)).join('\n'));
  try { console.error('--- calibration.txt ---\n' + fs.readFileSync(path.join(box, 'calibration.txt'), 'utf8')); } catch {}
  console.error('--- transcript replies ---\n' + fs.readFileSync(transcript, 'utf8').split('\n').filter(Boolean).map((l) => { const j = JSON.parse(l); return j.timestamp ? `${j.timestamp} ${j.message.usage.output_tokens * 5}` : ''; }).join('\n'));
  console.error('--- app.log ---\n' + appLog().split('\n').slice(-40).join('\n'));
  for (const name of Object.keys(phones)) console.error(`--- ${name} ---\n` + inbox(name).map((n) => `${n.title} | ${n.body}`).join('\n'));
  console.error('--- wrangler ---\n' + logs.split('\n').slice(-20).join('\n'));
  process.exitCode = 1;
} finally {
  if (app) app.kill();
  wrangler.kill();
  api.close();
  pushServer.close();
  fs.rmSync(devVars, { force: true });
  await sleep(1000);
  try { fs.rmSync(box, { recursive: true, force: true }); } catch {}
}
