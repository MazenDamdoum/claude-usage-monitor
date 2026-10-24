'use strict';
// Two HTTP servers, both bound to 127.0.0.1:
//
//   public   (Tailscale Funnel points here)  PWA files + /api/* for paired phones
//   internal (never funneled)                /internal/* for the Windows app and
//                                            the PowerShell scripts; bearer token
//                                            from bridge.json, loopback only.

const crypto = require('crypto');
const http = require('http');
const path = require('path');
const express = require('express');
const { NOTIFICATIONS, sanitizeSubscription } = require('./push');

const PAIR_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'; // no 0/O, 1/I
const PAIR_LENGTH = 8;
const MAX_PAIR_FAILURES = 5;
const TEST_COOLDOWN_MS = 10_000;

function sha256(s) {
  return crypto.createHash('sha256').update(String(s)).digest();
}

function safeEqual(a, b) {
  const x = Buffer.from(String(a));
  const y = Buffer.from(String(b));
  return x.length === y.length && crypto.timingSafeEqual(x, y);
}

// Fixed-window rate limiter (in memory; single process).
function rateLimit(max, windowMs) {
  let windowStart = Date.now();
  let count = 0;
  return (req, res, next) => {
    const now = Date.now();
    if (now - windowStart > windowMs) {
      windowStart = now;
      count = 0;
    }
    if (++count > max) return res.status(429).json({ error: 'Too many requests, try again shortly.' });
    next();
  };
}

function securityHeaders(req, res, next) {
  res.set({
    'X-Content-Type-Options': 'nosniff',
    'Referrer-Policy': 'no-referrer',
    'X-Frame-Options': 'DENY',
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Permissions-Policy': 'camera=(), microphone=(), geolocation=()',
    'Content-Security-Policy':
      "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; " +
      "connect-src 'self'; manifest-src 'self'; worker-src 'self'; frame-ancestors 'none'; " +
      "base-uri 'none'; form-action 'none'",
  });
  next();
}

function jsonErrors(err, req, res, next) {
  if (err && (err.type === 'entity.parse.failed' || err.type === 'entity.too.large')) {
    return res.status(400).json({ error: 'Invalid request body.' });
  }
  next(err);
}

const isInt = (v) => Number.isSafeInteger(v);

function normalizeWindow(w) {
  if (w === null || w === undefined) return null;
  if (typeof w !== 'object' || !isInt(w.utilization_x100) || !isInt(w.resets_at)) return undefined;
  if (w.utilization_x100 < 0 || w.utilization_x100 > 100_000 || w.resets_at < 0) return undefined;
  return { percent: w.utilization_x100 / 100, resetsAt: w.resets_at };
}

function createApps({ store, log, pusher, testOrigin, pairTtlMs = 10 * 60_000 }) {
  let pairing = null; // { codeHash, expiresAt, failures }

  // ---------------------------------------------------------------- public
  const pub = express();
  pub.disable('x-powered-by');
  pub.set('etag', false);
  pub.use(securityHeaders);

  pub.use(
    express.static(path.join(__dirname, '..', 'public'), {
      index: 'index.html',
      setHeaders(res, file) {
        if (file.endsWith('sw.js') || file.endsWith('.html') || file.endsWith('.webmanifest')) {
          res.set('Cache-Control', 'no-cache');
        }
      },
    }),
  );

  const api = express.Router();
  api.use(express.json({ limit: '16kb' }));

  api.post('/pair', rateLimit(10, 60_000), (req, res) => {
    const code = String((req.body && req.body.code) || '').toUpperCase().replace(/[^A-Z0-9]/g, '');
    if (!pairing || Date.now() > pairing.expiresAt) {
      pairing = null;
      return res.status(403).json({ error: 'No active pairing code. Run push\\pair.ps1 on your PC first.' });
    }
    if (code.length !== PAIR_LENGTH || !crypto.timingSafeEqual(sha256(code), pairing.codeHash)) {
      pairing.failures += 1;
      if (pairing.failures >= MAX_PAIR_FAILURES) {
        pairing = null;
        log.warn('pairing code locked after too many wrong attempts');
      }
      return res.status(403).json({ error: 'Wrong pairing code.' });
    }
    pairing = null; // one-time
    const name = String((req.body && req.body.name) || 'iPhone').replace(/[^\w .'-]/g, '').slice(0, 40) || 'iPhone';
    const { device, token } = store.addDevice(name);
    log.info(`paired device ${device.id} (${name})`);
    res.status(201).json({ token, deviceId: device.id });
  });

  // Everything below needs a paired device's bearer token.
  api.use((req, res, next) => {
    const m = /^Bearer (\S+)$/.exec(req.headers.authorization || '');
    const device = m && store.deviceForToken(m[1]);
    if (!device) return res.status(401).json({ error: 'This device is not paired.' });
    req.device = device;
    next();
  });

  api.get('/status', (req, res) => {
    const d = req.device;
    res.json({
      serverTime: Date.now(),
      usage: store.state.usage,
      usageReceivedAt: store.state.usageReceivedAt,
      vapidPublicKey: store.config.vapid.publicKey,
      device: { id: d.id, name: d.name, subscribed: !!d.subscription, prefs: d.prefs },
    });
  });

  api.post('/subscribe', (req, res) => {
    const sub = sanitizeSubscription(req.body && req.body.subscription, testOrigin);
    if (!sub) return res.status(400).json({ error: 'Invalid push subscription.' });
    for (const other of store.devices) {
      if (other !== req.device && other.subscription && other.subscription.endpoint === sub.endpoint) {
        other.subscription = null; // one device per browser subscription
      }
    }
    req.device.subscription = sub;
    store.saveDevices();
    log.info(`device ${req.device.id} subscribed`);
    res.json({ subscribed: true });
  });

  api.delete('/subscription', (req, res) => {
    req.device.subscription = null;
    store.saveDevices();
    res.json({ subscribed: false });
  });

  api.put('/prefs', (req, res) => {
    const body = req.body || {};
    for (const kind of [...Object.keys(NOTIFICATIONS), 'account_switch']) {
      if (kind in body) {
        if (typeof body[kind] !== 'boolean') return res.status(400).json({ error: `${kind} must be true or false.` });
        req.device.prefs[kind] = body[kind];
      }
    }
    store.saveDevices();
    res.json({ prefs: req.device.prefs });
  });

  api.post('/test', async (req, res) => {
    const d = req.device;
    if (!d.subscription) return res.status(409).json({ error: 'Enable notifications first.' });
    if (Date.now() - d.lastTestAt < TEST_COOLDOWN_MS) {
      return res.status(429).json({ error: 'Please wait a few seconds between tests.' });
    }
    d.lastTestAt = Date.now();
    const delivered = await pusher.sendTest([d]);
    log.info(`test notification to device ${d.id}: ${delivered ? 'sent' : 'failed'}`);
    if (!delivered) return res.status(502).json({ error: 'The push service did not accept the notification.' });
    res.json({ delivered });
  });

  api.post('/unpair', (req, res) => {
    store.removeDevice(req.device.id);
    log.info(`device ${req.device.id} unpaired itself`);
    res.status(204).end();
  });

  api.use((req, res) => res.status(404).json({ error: 'Not found.' }));
  pub.use('/api', api);
  pub.use(jsonErrors);
  pub.use((req, res) => res.status(404).type('text').send('Not found'));

  // -------------------------------------------------------------- internal
  const internal = express();
  internal.disable('x-powered-by');
  internal.set('etag', false);
  internal.use((req, res, next) => {
    const ip = req.socket.remoteAddress;
    if (ip !== '127.0.0.1' && ip !== '::1' && ip !== '::ffff:127.0.0.1') return res.status(403).end();
    const port = req.socket.localPort;
    const host = String(req.headers.host || '').toLowerCase();
    if (host !== `127.0.0.1:${port}` && host !== `localhost:${port}`) return res.status(403).end(); // DNS rebinding
    const m = /^Bearer ([0-9a-fA-F]{64})$/.exec(req.headers.authorization || '');
    if (!m || !safeEqual(m[1].toLowerCase(), store.config.localToken)) return res.status(401).end();
    next();
  });
  internal.use(express.json({ limit: '8kb' }));

  internal.get('/internal/health', (req, res) => {
    res.json({
      ok: true,
      devices: store.devices.length,
      subscribed: store.devices.filter((d) => d.subscription).length,
      usageReceivedAt: store.state.usageReceivedAt,
    });
  });

  internal.post('/internal/usage', (req, res) => {
    const b = req.body || {};
    const plan = typeof b.plan === 'string' && /^[\w-]{0,32}$/.test(b.plan) ? b.plan : null;
    const windows = {};
    for (const key of ['five_hour', 'seven_day', 'seven_day_sonnet', 'seven_day_opus']) {
      const w = normalizeWindow(b[key]);
      if (w === undefined) return res.status(400).json({ error: `invalid ${key}` });
      windows[key] = w;
    }
    if (!isInt(b.fetched_at) || plan === null) return res.status(400).json({ error: 'invalid usage' });
    store.state.usage = { plan, fetchedAt: b.fetched_at, ...windows };
    store.state.usageReceivedAt = Date.now();
    store.saveState();
    res.status(204).end();
  });

  internal.post('/internal/event', (req, res) => {
    const { id, kind, reset_at: resetAt } = req.body || {};
    const now = Math.floor(Date.now() / 1000);
    if (!NOTIFICATIONS[kind] || !isInt(resetAt) || resetAt < now - 30 * 86400 || resetAt > now + 86400 ||
        id !== `${kind}:${resetAt}`) {
      return res.status(400).json({ error: 'invalid event' });
    }
    if (store.hasSent(id)) {
      log.info(`event ${id} already sent; ignored`);
      return res.status(200).json({ duplicate: true });
    }
    store.markSent(id); // before sending: retries from the app can never double-notify
    res.status(202).json({ accepted: true });
    pusher
      .sendReset(kind)
      .then((n) => log.info(`event ${id}: notified ${n} device(s)`))
      .catch((err) => log.error(`event ${id}: ${err.message}`));
  });

  // One-off notices from the Windows app, e.g. an automatic account switch.
  internal.post('/internal/notice', (req, res) => {
    const { id, title, body } = req.body || {};
    if (typeof id !== 'string' || !/^[\w:.>-]{1,100}$/.test(id) || typeof title !== 'string' || !title ||
        title.length > 80 || typeof body !== 'string' || body.length > 240) {
      return res.status(400).json({ error: 'invalid notice' });
    }
    if (store.hasSent(`notice:${id}`)) return res.status(200).json({ duplicate: true });
    store.markSent(`notice:${id}`);
    res.status(202).json({ accepted: true });
    pusher
      .sendNotice(title, body)
      .then((n) => log.info(`notice ${id}: notified ${n} device(s)`))
      .catch((err) => log.error(`notice ${id}: ${err.message}`));
  });

  internal.post('/internal/pair', (req, res) => {
    let code = '';
    const bytes = crypto.randomBytes(PAIR_LENGTH);
    for (const b of bytes) code += PAIR_ALPHABET[b % PAIR_ALPHABET.length];
    pairing = { codeHash: sha256(code), expiresAt: Date.now() + pairTtlMs, failures: 0 };
    log.info('new pairing code issued');
    res.json({ code: `${code.slice(0, 4)}-${code.slice(4)}`, expiresAt: pairing.expiresAt });
  });

  internal.post('/internal/test', async (req, res) => {
    const targets = store.devices.filter((d) => d.subscription);
    const delivered = await pusher.sendTest(targets);
    log.info(`test notification from PC: ${delivered}/${targets.length} delivered`);
    res.json({ delivered, subscribed: targets.length });
  });

  internal.use(jsonErrors);
  internal.use((req, res) => res.status(404).end());

  return { pub, internal };
}

function listen(app, port) {
  return new Promise((resolve, reject) => {
    const server = http.createServer(app);
    server.keepAliveTimeout = 5000;
    server.headersTimeout = 10000;
    server.requestTimeout = 15000;
    server.once('error', reject);
    server.listen(port, '127.0.0.1', () => resolve(server));
  });
}

module.exports = { createApps, listen };
