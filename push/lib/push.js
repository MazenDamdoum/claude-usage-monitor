'use strict';
// Web Push delivery. Subscriptions may only point at real browser push services
// (no arbitrary URLs: the server would otherwise make requests anywhere).

const http = require('http');
const https = require('https');
const webpush = require('web-push');
const { redactEndpoint } = require('./log');

// web-push builds the encrypted, VAPID-signed request; we send it ourselves so the
// timeout is ours and tests can point at a local plain-HTTP fake push service.
function deliver(details, allowHttp) {
  return new Promise((resolve, reject) => {
    const url = new URL(details.endpoint);
    const client = url.protocol === 'https:' ? https : allowHttp && url.protocol === 'http:' ? http : null;
    if (!client) return reject(new Error('unsupported endpoint protocol'));
    const req = client.request(url, { method: details.method, headers: details.headers, timeout: 10000 }, (res) => {
      res.resume();
      res.on('end', () => resolve(res.statusCode));
    });
    req.on('timeout', () => req.destroy(new Error('timeout')));
    req.on('error', reject);
    req.end(details.body);
  });
}

const PUSH_HOSTS = [
  'web.push.apple.com',            // Safari / iOS / iPadOS / macOS
  'fcm.googleapis.com',            // Chrome, Edge on Android
  'updates.push.services.mozilla.com',
  'notify.windows.com',            // Edge on Windows (wns2-*.notify.windows.com)
];

const NOTIFICATIONS = {
  five_hour_reset: {
    title: 'Claude 5-Hour Limit Reset',
    body: 'Your usage window has reset.',
  },
  weekly_reset: {
    title: 'Claude Weekly Limit Reset',
    body: 'Your weekly usage window has reset.',
  },
};

function isAllowedEndpoint(endpoint, testOrigin) {
  let url;
  try {
    url = new URL(endpoint);
  } catch {
    return false;
  }
  if (testOrigin && url.origin === testOrigin) return true;
  if (url.protocol !== 'https:' || url.username || url.password || url.port) return false;
  return PUSH_HOSTS.some((h) => url.hostname === h || url.hostname.endsWith(`.${h}`));
}

const B64URL = /^[A-Za-z0-9_-]+={0,2}$/;

// Returns a clean {endpoint, keys:{p256dh, auth}} or null.
function sanitizeSubscription(sub, testOrigin) {
  if (!sub || typeof sub !== 'object') return null;
  const { endpoint, keys } = sub;
  if (typeof endpoint !== 'string' || endpoint.length > 1024) return null;
  if (!isAllowedEndpoint(endpoint, testOrigin)) return null;
  if (!keys || typeof keys.p256dh !== 'string' || typeof keys.auth !== 'string') return null;
  if (keys.p256dh.length < 40 || keys.p256dh.length > 200 || !B64URL.test(keys.p256dh)) return null;
  if (keys.auth.length < 8 || keys.auth.length > 64 || !B64URL.test(keys.auth)) return null;
  return { endpoint, keys: { p256dh: keys.p256dh, auth: keys.auth } };
}

class Pusher {
  constructor(store, log, { subject, testOrigin } = {}) {
    this.store = store;
    this.log = log;
    this.testOrigin = testOrigin;
    this.vapid = {
      subject: subject || 'mailto:claude-usage-monitor@users.noreply.github.com',
      publicKey: store.config.vapid.publicKey,
      privateKey: store.config.vapid.privateKey,
    };
  }

  // Sends to one device. Resolves true on success. Removes dead subscriptions.
  async sendTo(device, notification, { ttl = 6 * 3600, topic } = {}) {
    if (!device.subscription) return false;
    const payload = JSON.stringify({
      title: notification.title,
      body: notification.body,
      tag: notification.tag,
      url: '/',
      timestamp: Date.now(),
    });
    const options = { vapidDetails: this.vapid, TTL: ttl, urgency: 'high' };
    if (topic) options.topic = topic;

    for (let attempt = 1; attempt <= 2; attempt++) {
      try {
        const details = webpush.generateRequestDetails(device.subscription, payload, options);
        const allowHttp = !!this.testOrigin && new URL(details.endpoint).origin === this.testOrigin;
        const code = await deliver(details, allowHttp);
        if (code >= 200 && code < 300) return true;
        throw Object.assign(new Error(`HTTP ${code}`), { statusCode: code });
      } catch (err) {
        const status = err && err.statusCode;
        const host = redactEndpoint(device.subscription.endpoint);
        if (status === 404 || status === 410) {
          this.log.warn(`subscription for device ${device.id} expired (${status} from ${host}); removed`);
          device.subscription = null;
          this.store.saveDevices();
          return false;
        }
        const retryable = !status || status >= 500 || status === 429;
        if (!retryable || attempt === 2) {
          this.log.error(`push to device ${device.id} via ${host} failed: ${status || err.code || err.message}`);
          return false;
        }
        await new Promise((r) => setTimeout(r, 3000));
      }
    }
    return false;
  }

  // Reset notification to every subscribed device that wants this kind.
  async sendReset(kind) {
    const base = NOTIFICATIONS[kind];
    const targets = this.store.devices.filter((d) => d.subscription && d.prefs[kind] !== false);
    const results = await Promise.all(
      targets.map((d) => this.sendTo(d, { ...base, tag: kind }, { topic: kind })),
    );
    return results.filter(Boolean).length;
  }

  async sendNotice(title, body) {
    const targets = this.store.devices.filter((d) => d.subscription && d.prefs.account_switch !== false);
    const results = await Promise.all(
      targets.map((d) => this.sendTo(d, { title, body, tag: 'account_switch' }, { topic: 'account_switch' })),
    );
    return results.filter(Boolean).length;
  }

  async sendTest(devices) {
    const note = { title: 'Claude Usage Monitor', body: 'Test notification: phone alerts are working.', tag: 'test' };
    const results = await Promise.all(devices.map((d) => this.sendTo(d, note, { ttl: 600 })));
    return results.filter(Boolean).length;
  }
}

module.exports = { Pusher, sanitizeSubscription, isAllowedEndpoint, NOTIFICATIONS };
