'use strict';
// Persistent state, kept outside the repository (default
// %APPDATA%\ClaudeUsageMonitor\push). JSON files are written atomically.
//
//   config.json   VAPID key pair + local bridge token   (secret)
//   devices.json  paired devices and their push subscriptions (secret)
//   state.json    latest usage snapshot + already-sent reset events
//   bridge.json   { port, token } for the Windows app   (secret)

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const webpush = require('web-push');

const MAX_SENT_EVENTS = 200;

function defaultDataDir() {
  if (process.env.CUM_PUSH_DATA) return process.env.CUM_PUSH_DATA;
  const base = process.env.APPDATA || path.join(require('os').homedir(), '.config');
  return path.join(base, 'ClaudeUsageMonitor', 'push');
}

function readJson(file, fallback) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch {
    return fallback;
  }
}

function writeJson(file, value) {
  const tmp = `${file}.${process.pid}.tmp`;
  fs.writeFileSync(tmp, JSON.stringify(value, null, 2), { mode: 0o600 });
  fs.renameSync(tmp, file);
}

class Store {
  constructor(dataDir) {
    this.dir = dataDir;
    fs.mkdirSync(dataDir, { recursive: true });
    this.files = {
      config: path.join(dataDir, 'config.json'),
      devices: path.join(dataDir, 'devices.json'),
      state: path.join(dataDir, 'state.json'),
      bridge: path.join(dataDir, 'bridge.json'),
    };

    this.config = readJson(this.files.config, null);
    if (!this.config || !this.config.vapid || !this.config.localToken) {
      const vapid = webpush.generateVAPIDKeys();
      this.config = {
        version: 1,
        vapid: { publicKey: vapid.publicKey, privateKey: vapid.privateKey },
        localToken: crypto.randomBytes(32).toString('hex'),
        createdAt: new Date().toISOString(),
      };
      writeJson(this.files.config, this.config);
      this.createdKeys = true;
    }

    this.devices = readJson(this.files.devices, { devices: [] }).devices || [];
    const state = readJson(this.files.state, {});
    this.state = {
      usage: state.usage || null,
      usageReceivedAt: state.usageReceivedAt || 0,
      sentEvents: Array.isArray(state.sentEvents) ? state.sentEvents : [],
    };
  }

  writeBridge(port) {
    writeJson(this.files.bridge, { port, token: this.config.localToken });
  }

  saveDevices() {
    writeJson(this.files.devices, { devices: this.devices });
  }

  saveState() {
    writeJson(this.files.state, this.state);
  }

  // ---- devices ----------------------------------------------------------

  static hashToken(token) {
    return crypto.createHash('sha256').update(String(token)).digest('hex');
  }

  addDevice(name) {
    const token = crypto.randomBytes(32).toString('base64url');
    const device = {
      id: crypto.randomBytes(8).toString('hex'),
      name,
      tokenHash: Store.hashToken(token),
      createdAt: new Date().toISOString(),
      subscription: null,
      prefs: { five_hour_reset: true, weekly_reset: true, account_switch: true },
      lastTestAt: 0,
    };
    this.devices.push(device);
    this.saveDevices();
    return { device, token };
  }

  deviceForToken(token) {
    if (typeof token !== 'string' || token.length < 20 || token.length > 200) return null;
    const hash = Store.hashToken(token);
    return this.devices.find((d) => d.tokenHash === hash) || null;
  }

  removeDevice(id) {
    this.devices = this.devices.filter((d) => d.id !== id);
    this.saveDevices();
  }

  // ---- reset events -----------------------------------------------------

  hasSent(id) {
    return this.state.sentEvents.some((e) => e.id === id);
  }

  markSent(id) {
    this.state.sentEvents.push({ id, at: Date.now() });
    if (this.state.sentEvents.length > MAX_SENT_EVENTS) {
      this.state.sentEvents = this.state.sentEvents.slice(-MAX_SENT_EVENTS);
    }
    this.saveState();
  }
}

module.exports = { Store, defaultDataDir };
