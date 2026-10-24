'use strict';
// End-to-end tests for the push server: startup/config, PWA readiness, auth,
// pairing, subscriptions, encrypted delivery, reset events, de-duplication,
// restart persistence and expired subscriptions. A local fake push service
// stands in for Apple's, and payloads are decrypted to prove they're valid.

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('crypto');
const fs = require('fs');
const http = require('http');
const os = require('os');
const path = require('path');
const { execFile } = require('child_process');
const ece = require('http_ece');
const { start } = require('../server');

const ROOT = path.join(__dirname, '..');

// ---- fake push service ---------------------------------------------------------------

function startFakePushService() {
  const received = [];
  const server = http.createServer((req, res) => {
    const chunks = [];
    req.on('data', (c) => chunks.push(c));
    req.on('end', () => {
      received.push({ path: req.url, headers: req.headers, body: Buffer.concat(chunks) });
      res.statusCode = req.url.startsWith('/gone') ? 410 : 201;
      res.end();
    });
  });
  return new Promise((resolve) =>
    server.listen(0, '127.0.0.1', () => resolve({ server, received, origin: `http://127.0.0.1:${server.address().port}` })),
  );
}

function makeSubscription(origin, pathName) {
  const ecdh = crypto.createECDH('prime256v1');
  ecdh.generateKeys();
  const auth = crypto.randomBytes(16);
  return {
    ecdh,
    auth,
    json: {
      endpoint: `${origin}${pathName}`,
      keys: { p256dh: ecdh.getPublicKey().toString('base64url'), auth: auth.toString('base64url') },
    },
  };
}

function decrypt(sub, body) {
  const plain = ece.decrypt(body, { version: 'aes128gcm', privateKey: sub.ecdh, authSecret: sub.auth });
  return JSON.parse(plain.toString('utf8'));
}

async function waitFor(fn, ms = 5000) {
  const until = Date.now() + ms;
  while (Date.now() < until) {
    const v = fn();
    if (v) return v;
    await new Promise((r) => setTimeout(r, 25));
  }
  throw new Error('timed out');
}

// ---- helpers -------------------------------------------------------------------

function tmpDir() {
  return fs.mkdtempSync(path.join(os.tmpdir(), 'cum-push-test-'));
}

async function request(port, method, url, { body, token, headers = {} } = {}) {
  const res = await fetch(`http://127.0.0.1:${port}${url}`, {
    method,
    headers: {
      ...(body !== undefined ? { 'Content-Type': 'application/json' } : {}),
      ...(token ? { Authorization: `Bearer ${token}` } : {}),
      ...headers,
    },
    body: body === undefined ? undefined : typeof body === 'string' ? body : JSON.stringify(body),
  });
  const text = await res.text();
  let json = null;
  try {
    json = JSON.parse(text);
  } catch {}
  return { status: res.status, headers: res.headers, text, json };
}

function rawRequest(port, { method = 'GET', url = '/', headers = {} }) {
  return new Promise((resolve, reject) => {
    const req = http.request({ host: '127.0.0.1', port, method, path: url, headers }, (res) => {
      res.resume();
      res.on('end', () => resolve(res.statusCode));
    });
    req.on('error', reject);
    req.end();
  });
}

// ---- tests -----------------------------------------------------------------------

describe('push server', () => {
  let fake;
  let dataDir;
  let srv;
  let localToken;
  let deviceToken;
  let sub;

  const internal = (method, url, body) => request(srv.internalPort, method, url, { body, token: localToken });
  const app = (method, url, body, token = deviceToken) => request(srv.publicPort, method, url, { body, token });

  async function startServer(extra = {}) {
    srv = await start({ dataDir, publicPort: 0, internalPort: 0, quiet: true, testOrigin: fake.origin, ...extra });
    localToken = JSON.parse(fs.readFileSync(path.join(dataDir, 'bridge.json'), 'utf8')).token;
  }

  before(async () => {
    fake = await startFakePushService();
    dataDir = tmpDir();
    await startServer();
  });

  after(async () => {
    if (srv) await srv.close();
    fake.server.close();
  });

  describe('startup and configuration', () => {
    it('creates VAPID keys, a local token and bridge.json', () => {
      const config = JSON.parse(fs.readFileSync(path.join(dataDir, 'config.json'), 'utf8'));
      assert.match(config.vapid.publicKey, /^[A-Za-z0-9_-]{80,}$/);
      assert.match(config.vapid.privateKey, /^[A-Za-z0-9_-]{40,}$/);
      assert.match(config.localToken, /^[0-9a-f]{64}$/);
      const bridge = JSON.parse(fs.readFileSync(path.join(dataDir, 'bridge.json'), 'utf8'));
      assert.equal(bridge.port, srv.internalPort);
      assert.equal(bridge.token, config.localToken);
    });

    it('never writes secrets to the log', () => {
      const log = fs.readFileSync(path.join(dataDir, 'server.log'), 'utf8');
      const config = JSON.parse(fs.readFileSync(path.join(dataDir, 'config.json'), 'utf8'));
      assert.ok(!log.includes(config.localToken));
      assert.ok(!log.includes(config.vapid.privateKey));
    });

    it('starts as a standalone process and reports readiness', async () => {
      const dir = tmpDir();
      const out = await new Promise((resolve, reject) => {
        const child = execFile(process.execPath, ['server.js'], {
          cwd: ROOT,
          env: { ...process.env, CUM_PUSH_DATA: dir, CUM_PUSH_PORT: '0', CUM_PUSH_INTERNAL_PORT: '0' },
        });
        let text = '';
        child.stdout.on('data', (d) => {
          text += d;
          if (text.includes('ready:')) {
            child.kill();
            resolve(text);
          }
        });
        child.on('error', reject);
        setTimeout(() => reject(new Error(`no ready line: ${text}`)), 8000);
      });
      assert.match(out, /ready: app http:\/\/127\.0\.0\.1:\d+/);
      assert.ok(fs.existsSync(path.join(dir, 'bridge.json')));
    });
  });

  describe('iOS PWA readiness', () => {
    it('serves the page with iOS home-screen metadata and security headers', async () => {
      const res = await app('GET', '/', undefined, null);
      assert.equal(res.status, 200);
      assert.match(res.text, /<link rel="manifest" href="\/manifest.webmanifest">/);
      assert.match(res.text, /<link rel="apple-touch-icon" href="\/icons\/apple-touch-icon.png">/);
      assert.match(res.text, /name="apple-mobile-web-app-capable" content="yes"/);
      assert.match(res.headers.get('content-security-policy'), /default-src 'self'/);
      assert.equal(res.headers.get('x-content-type-options'), 'nosniff');
      assert.equal(res.headers.get('x-powered-by'), null);
    });

    it('has an installable manifest whose icons exist at the declared sizes', async () => {
      const res = await app('GET', '/manifest.webmanifest', undefined, null);
      assert.equal(res.status, 200);
      assert.match(res.headers.get('content-type'), /application\/manifest\+json/);
      const m = res.json;
      assert.equal(m.display, 'standalone');
      assert.equal(m.start_url, '/');
      assert.ok(m.name && m.short_name);
      const sizes = [];
      for (const icon of m.icons) {
        const png = await fetch(`http://127.0.0.1:${srv.publicPort}${icon.src}`);
        assert.equal(png.status, 200, icon.src);
        const buf = Buffer.from(await png.arrayBuffer());
        const w = buf.readUInt32BE(16);
        const h = buf.readUInt32BE(20);
        assert.equal(`${w}x${h}`, icon.sizes, icon.src);
        sizes.push(icon.sizes);
      }
      assert.ok(sizes.includes('192x192') && sizes.includes('512x512'));
      const apple = await fetch(`http://127.0.0.1:${srv.publicPort}/icons/apple-touch-icon.png`);
      assert.equal(apple.status, 200);
    });

    it('serves a service worker that shows pushes and is not cached', async () => {
      const res = await app('GET', '/sw.js', undefined, null);
      assert.equal(res.status, 200);
      assert.match(res.headers.get('content-type'), /javascript/);
      assert.match(res.headers.get('cache-control'), /no-cache/);
      assert.match(res.text, /addEventListener\('push'/);
      assert.match(res.text, /showNotification/);
    });
  });

  describe('authentication and security', () => {
    it('rejects API calls from unpaired devices', async () => {
      assert.equal((await app('GET', '/api/status', undefined, null)).status, 401);
      assert.equal((await app('GET', '/api/status', undefined, 'x'.repeat(43))).status, 401);
      assert.equal((await app('POST', '/api/test', {}, null)).status, 401);
    });

    it('does not expose internal endpoints on the public port', async () => {
      const res = await request(srv.publicPort, 'POST', '/internal/event', {
        token: localToken,
        body: { id: 'weekly_reset:1', kind: 'weekly_reset', reset_at: 1 },
      });
      assert.equal(res.status, 404);
    });

    it('requires the local token on the internal port', async () => {
      assert.equal((await request(srv.internalPort, 'GET', '/internal/health')).status, 401);
      assert.equal((await request(srv.internalPort, 'GET', '/internal/health', { token: 'a'.repeat(64) })).status, 401);
      assert.equal((await internal('GET', '/internal/health')).status, 200);
    });

    it('rejects internal requests with a foreign Host header (DNS rebinding)', async () => {
      const status = await rawRequest(srv.internalPort, {
        url: '/internal/health',
        headers: { Host: 'evil.example.com', Authorization: `Bearer ${localToken}` },
      });
      assert.equal(status, 403);
    });

    it('rejects malformed JSON without leaking details', async () => {
      const res = await request(srv.internalPort, 'POST', '/internal/usage', { token: localToken, body: '{oops' });
      assert.equal(res.status, 400);
      assert.ok(!/at .*\.js/.test(res.text));
    });
  });

  describe('pairing', () => {
    it('refuses pairing when no code was issued', async () => {
      assert.equal((await app('POST', '/api/pair', { code: 'ABCD-EFGH' }, null)).status, 403);
    });

    it('locks a code after repeated wrong guesses', async () => {
      const { json } = await internal('POST', '/internal/pair');
      for (let i = 0; i < 5; i++) {
        assert.equal((await app('POST', '/api/pair', { code: 'ZZZZ-ZZZZ' }, null)).status, 403);
      }
      assert.equal((await app('POST', '/api/pair', { code: json.code }, null)).status, 403);
    });

    it('pairs once with a fresh code', async () => {
      const { json } = await internal('POST', '/internal/pair');
      assert.match(json.code, /^[A-Z2-9]{4}-[A-Z2-9]{4}$/);
      const res = await app('POST', '/api/pair', { code: json.code.toLowerCase(), name: 'iPhone' }, null);
      assert.equal(res.status, 201);
      deviceToken = res.json.token;
      assert.ok(deviceToken.length >= 40);
      assert.equal((await app('POST', '/api/pair', { code: json.code }, null)).status, 403, 'code is one-time');
      const devices = JSON.parse(fs.readFileSync(path.join(dataDir, 'devices.json'), 'utf8')).devices;
      assert.ok(!JSON.stringify(devices).includes(deviceToken), 'only a hash of the token is stored');
    });

    it('expires codes', async () => {
      await srv.close();
      await startServer({ pairTtlMs: 50 });
      const { json } = await internal('POST', '/internal/pair');
      await new Promise((r) => setTimeout(r, 120));
      assert.equal((await app('POST', '/api/pair', { code: json.code }, null)).status, 403);
    });
  });

  describe('subscriptions and delivery', () => {
    it('refuses subscriptions that do not point at a push service', async () => {
      const bad = makeSubscription('https://evil.example.com', '/x').json;
      assert.equal((await app('POST', '/api/subscribe', { subscription: bad })).status, 400);
      const internalHost = makeSubscription('http://127.0.0.1:1', '/x').json;
      assert.equal((await app('POST', '/api/subscribe', { subscription: internalHost })).status, 400);
      assert.equal((await app('POST', '/api/subscribe', { subscription: { endpoint: 'nope' } })).status, 400);
    });

    it('accepts a valid subscription', async () => {
      sub = makeSubscription(fake.origin, '/device-1');
      const res = await app('POST', '/api/subscribe', { subscription: sub.json });
      assert.equal(res.status, 200);
      const status = await app('GET', '/api/status');
      assert.equal(status.json.device.subscribed, true);
      assert.ok(status.json.vapidPublicKey);
    });

    it('delivers an encrypted, VAPID-signed test notification', async () => {
      const before = fake.received.length;
      const res = await app('POST', '/api/test');
      assert.equal(res.status, 200);
      const msg = fake.received[before];
      assert.equal(msg.path, '/device-1');
      assert.match(msg.headers.authorization, /^vapid t=.+, k=.+/);
      assert.equal(msg.headers['content-encoding'], 'aes128gcm');
      assert.ok(Number(msg.headers.ttl) > 0);
      assert.equal(msg.headers.urgency, 'high');
      const payload = decrypt(sub, msg.body);
      assert.equal(payload.title, 'Claude Usage Monitor');
    });

    it('rate-limits test notifications', async () => {
      assert.equal((await app('POST', '/api/test')).status, 429);
    });
  });

  describe('reset events', () => {
    const now = () => Math.floor(Date.now() / 1000);
    let fiveId;

    it('stores usage from the Windows app and shows it to the phone', async () => {
      const usage = {
        fetched_at: now(),
        plan: 'pro',
        five_hour: { utilization_x100: 6650, resets_at: now() + 3600 },
        seven_day: { utilization_x100: 1200, resets_at: now() + 86400 },
        seven_day_sonnet: null,
        seven_day_opus: null,
      };
      assert.equal((await internal('POST', '/internal/usage', usage)).status, 204);
      const status = await app('GET', '/api/status');
      assert.equal(status.json.usage.five_hour.percent, 66.5);
      assert.equal(status.json.usage.seven_day.resetsAt, usage.seven_day.resets_at);
      assert.equal((await internal('POST', '/internal/usage', { ...usage, five_hour: { utilization_x100: 'x' } })).status, 400);
    });

    it('sends the 5-hour reset notification', async () => {
      const resetAt = now() - 10;
      fiveId = `five_hour_reset:${resetAt}`;
      const before = fake.received.length;
      const res = await internal('POST', '/internal/event', { id: fiveId, kind: 'five_hour_reset', reset_at: resetAt });
      assert.equal(res.status, 202);
      await waitFor(() => fake.received.length > before);
      const payload = decrypt(sub, fake.received[before].body);
      assert.equal(payload.title, 'Claude 5-Hour Limit Reset');
      assert.equal(payload.body, 'Your usage window has reset.');
    });

    it('sends the weekly reset notification', async () => {
      const resetAt = now() - 20;
      const before = fake.received.length;
      const res = await internal('POST', '/internal/event', {
        id: `weekly_reset:${resetAt}`, kind: 'weekly_reset', reset_at: resetAt,
      });
      assert.equal(res.status, 202);
      await waitFor(() => fake.received.length > before);
      const payload = decrypt(sub, fake.received[before].body);
      assert.equal(payload.title, 'Claude Weekly Limit Reset');
      assert.equal(payload.body, 'Your weekly usage window has reset.');
    });

    it('never sends the same event twice', async () => {
      const before = fake.received.length;
      const [, resetAt] = fiveId.split(':');
      const res = await internal('POST', '/internal/event', { id: fiveId, kind: 'five_hour_reset', reset_at: Number(resetAt) });
      assert.equal(res.status, 200);
      assert.equal(res.json.duplicate, true);
      await new Promise((r) => setTimeout(r, 200));
      assert.equal(fake.received.length, before);
    });

    it('remembers sent events and keys across a restart', async () => {
      const keysBefore = fs.readFileSync(path.join(dataDir, 'config.json'), 'utf8');
      await srv.close();
      await startServer();
      assert.equal(fs.readFileSync(path.join(dataDir, 'config.json'), 'utf8'), keysBefore);
      const before = fake.received.length;
      const [, resetAt] = fiveId.split(':');
      const res = await internal('POST', '/internal/event', { id: fiveId, kind: 'five_hour_reset', reset_at: Number(resetAt) });
      assert.equal(res.json.duplicate, true);
      assert.equal((await app('GET', '/api/status')).status, 200, 'device still paired after restart');
      await new Promise((r) => setTimeout(r, 200));
      assert.equal(fake.received.length, before);
    });

    it('respects per-kind preferences', async () => {
      assert.equal((await app('PUT', '/api/prefs', { weekly_reset: false })).status, 200);
      assert.equal((await app('PUT', '/api/prefs', { weekly_reset: 'no' })).status, 400);
      const resetAt = now() - 30;
      const before = fake.received.length;
      await internal('POST', '/internal/event', { id: `weekly_reset:${resetAt}`, kind: 'weekly_reset', reset_at: resetAt });
      await new Promise((r) => setTimeout(r, 300));
      assert.equal(fake.received.length, before);
      await app('PUT', '/api/prefs', { weekly_reset: true });
    });

    it('sends account-switch notices once', async () => {
      const before = fake.received.length;
      const notice = { id: 'switch:1', title: 'Claude account switched', body: 'nayef used up its 5-hour limit. Now using huey.' };
      assert.equal((await internal('POST', '/internal/notice', notice)).status, 202);
      await waitFor(() => fake.received.length > before);
      assert.equal(decrypt(sub, fake.received[before].body).body, notice.body);
      assert.equal((await internal('POST', '/internal/notice', notice)).json.duplicate, true);
      assert.equal((await internal('POST', '/internal/notice', { ...notice, id: 'bad id!' })).status, 400);
    });

    it('validates events', async () => {
      const t = now();
      assert.equal((await internal('POST', '/internal/event', { id: `x:${t}`, kind: 'x', reset_at: t })).status, 400);
      assert.equal((await internal('POST', '/internal/event', { id: 'five_hour_reset:1', kind: 'five_hour_reset', reset_at: t })).status, 400);
      assert.equal((await internal('POST', '/internal/event', { id: 'five_hour_reset:5', kind: 'five_hour_reset', reset_at: 5 })).status, 400);
    });

    it('removes subscriptions the push service reports as gone', async () => {
      const gone = makeSubscription(fake.origin, '/gone-device');
      assert.equal((await app('POST', '/api/subscribe', { subscription: gone.json })).status, 200);
      const resetAt = now() - 40;
      await internal('POST', '/internal/event', { id: `five_hour_reset:${resetAt}`, kind: 'five_hour_reset', reset_at: resetAt });
      let subscribed = true;
      for (let i = 0; i < 100 && subscribed; i++) {
        await new Promise((r) => setTimeout(r, 30));
        subscribed = (await app('GET', '/api/status')).json.device.subscribed;
      }
      assert.equal(subscribed, false);
    });
  });

  describe('Windows app bridge', () => {
    const exe = path.join(ROOT, '..', 'build', 'push_bridge_test.exe');

    it('the C++ bridge delivers usage and events to the server', { skip: !fs.existsSync(exe) && 'build the C++ tests first (scripts\\test.ps1)' }, async () => {
      sub = makeSubscription(fake.origin, '/device-2');
      assert.equal((await app('POST', '/api/subscribe', { subscription: sub.json })).status, 200);
      const env = { ...process.env, CUM_PUSH_BRIDGE_FILE: path.join(dataDir, 'bridge.json') };
      const run = (what) =>
        new Promise((resolve) => execFile(exe, ['--post', what], { env }, (err, stdout) => resolve({ code: err ? err.code : 0, stdout })));

      const usage = await run('usage');
      assert.equal(usage.code, 0, usage.stdout);
      assert.equal((await app('GET', '/api/status')).json.usage.five_hour.percent, 42);

      const before = fake.received.length;
      const event = await run('event');
      assert.equal(event.code, 0, event.stdout);
      await waitFor(() => fake.received.length > before);
      assert.equal(decrypt(sub, fake.received[before].body).title, 'Claude 5-Hour Limit Reset');
    });
  });
});
