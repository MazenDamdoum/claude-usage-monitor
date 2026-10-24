'use strict';

const TOKEN_KEY = 'claude-usage.device-token';
const WARN = 80;
const CRITICAL = 95;

const $ = (id) => document.getElementById(id);
let token = localStorage.getItem(TOKEN_KEY);
let status = null;
let registration = null;

const isIOS = /iPhone|iPad|iPod/.test(navigator.userAgent) ||
  (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
const isStandalone = window.matchMedia('(display-mode: standalone)').matches || navigator.standalone === true;
const pushSupported = 'serviceWorker' in navigator && 'PushManager' in window && 'Notification' in window;

function show(id) {
  for (const s of ['install', 'pair', 'dashboard']) $(s).hidden = s !== id;
}

function message(text, isError = false) {
  const m = $('message');
  m.textContent = text || '';
  m.className = isError ? 'error center' : 'muted center';
}

async function api(method, path, body) {
  const res = await fetch(`/api${path}`, {
    method,
    headers: {
      'Content-Type': 'application/json',
      ...(token ? { Authorization: `Bearer ${token}` } : {}),
    },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  if (res.status === 401 && path !== '/pair') {
    localStorage.removeItem(TOKEN_KEY);
    token = null;
    show('pair');
    throw new Error('This iPhone is no longer paired.');
  }
  const data = res.status === 204 ? {} : await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || `Request failed (${res.status})`);
  return data;
}

// ---- formatting --------------------------------------------------------------

function duration(seconds) {
  if (seconds <= 0) return 'now';
  if (seconds < 60) return '<1m';
  const m = Math.floor(seconds / 60);
  if (m < 60) return `${m}m`;
  const h = Math.floor(m / 60);
  if (h < 24) return m % 60 ? `${h}h ${m % 60}m` : `${h}h`;
  return h % 24 ? `${Math.floor(h / 24)}d ${h % 24}h` : `${Math.floor(h / 24)}d`;
}

function clock(unixSeconds) {
  return new Date(unixSeconds * 1000).toLocaleString([], { weekday: 'short', hour: 'numeric', minute: '2-digit' });
}

function severity(pct) {
  return pct >= CRITICAL ? 'critical' : pct >= WARN ? 'warn' : '';
}

// ---- rendering -----------------------------------------------------------------

function renderLimit(key) {
  const card = $(`limit-${key}`);
  const w = status && status.usage && status.usage[key];
  const value = card.querySelector('.value');
  const fill = card.querySelector('.fill');
  const reset = card.querySelector('.reset');
  if (!w) {
    value.textContent = '--';
    value.className = 'value';
    fill.style.width = '0';
    reset.textContent = status && status.usage ? 'Not on your plan' : 'Waiting for the PC monitor';
    return;
  }
  const pct = Math.round(w.percent);
  value.textContent = `${pct}%`;
  value.className = `value ${severity(pct)}`;
  fill.style.width = `${Math.min(100, Math.max(0, w.percent))}%`;
  fill.className = `fill ${severity(pct)}`;
  const now = Date.now() / 1000;
  reset.textContent = w.resetsAt > 0 ? `Resets in ${duration(w.resetsAt - now)} · ${clock(w.resetsAt)}` : 'No active window';
}

function renderStatus() {
  if (!status) return;
  renderLimit('five_hour');
  renderLimit('seven_day');

  const age = status.usageReceivedAt ? Math.round((Date.now() - status.usageReceivedAt) / 1000) : null;
  $('monitor-status').textContent =
    age === null ? 'Not connected' : age < 180 ? `Live · updated ${duration(age) === '<1m' ? 'just now' : duration(age) + ' ago'}` : `Last update ${duration(age)} ago`;

  const permission = pushSupported ? Notification.permission : 'unsupported';
  let text;
  if (!pushSupported) text = isIOS && !isStandalone ? 'Add to Home Screen first' : 'Not supported here';
  else if (permission === 'denied') text = 'Blocked in Settings';
  else if (status.device.subscribed && permission === 'granted') text = 'On';
  else text = 'Off';
  $('notify-status').textContent = text;

  const on = text === 'On';
  $('enable').hidden = on;
  $('test').disabled = !on;
  for (const kind of ['five_hour_reset', 'weekly_reset', 'account_switch']) {
    const box = $(`pref-${kind}`);
    box.checked = status.device.prefs[kind] !== false;
    box.disabled = !on;
  }
}

async function refresh() {
  try {
    status = await api('GET', '/status');
    renderStatus();
    await healSubscription();
  } catch (err) {
    message(err.message, true);
  }
}

// If the browser still has a subscription the server lost (or vice versa), fix it quietly.
async function healSubscription() {
  if (!pushSupported || !registration || Notification.permission !== 'granted') return;
  const sub = await registration.pushManager.getSubscription();
  if (sub && !status.device.subscribed) {
    await api('POST', '/subscribe', { subscription: sub.toJSON() });
    status.device.subscribed = true;
    renderStatus();
  }
}

function urlBase64ToUint8Array(base64) {
  const padded = (base64 + '='.repeat((4 - (base64.length % 4)) % 4)).replace(/-/g, '+').replace(/_/g, '/');
  return Uint8Array.from(atob(padded), (c) => c.charCodeAt(0));
}

// Must run directly from a tap: iOS only allows permission prompts from user gestures.
async function enableNotifications() {
  message('');
  try {
    if (!pushSupported) throw new Error('Open Claude Usage from your Home Screen to enable notifications.');
    const permission = await Notification.requestPermission();
    if (permission !== 'granted') {
      throw new Error('Notifications are blocked. Allow them in Settings > Notifications > Claude Usage.');
    }
    registration = registration || (await navigator.serviceWorker.ready);
    let sub = await registration.pushManager.getSubscription();
    if (!sub) {
      sub = await registration.pushManager.subscribe({
        userVisibleOnly: true,
        applicationServerKey: urlBase64ToUint8Array(status.vapidPublicKey),
      });
    }
    await api('POST', '/subscribe', { subscription: sub.toJSON() });
    message('Notifications are on.');
    await refresh();
  } catch (err) {
    message(err.message, true);
  }
}

async function sendTest() {
  message('Sending…');
  try {
    await api('POST', '/test');
    message('Test sent. It should arrive in a few seconds.');
  } catch (err) {
    message(err.message, true);
  }
}

async function setPref(kind, value) {
  try {
    const { prefs } = await api('PUT', '/prefs', { [kind]: value });
    status.device.prefs = prefs;
  } catch (err) {
    message(err.message, true);
  }
  renderStatus();
}

async function unpair() {
  if (!confirm('Unpair this iPhone? It will stop receiving alerts.')) return;
  try {
    if (registration) {
      const sub = await registration.pushManager.getSubscription();
      if (sub) await sub.unsubscribe();
    }
    await api('POST', '/unpair');
  } catch {
    // ignore: unpairing locally is what matters
  }
  localStorage.removeItem(TOKEN_KEY);
  token = null;
  show('pair');
}

async function pair(event) {
  event.preventDefault();
  const err = $('pair-error');
  err.hidden = true;
  try {
    const { token: newToken } = await api('POST', '/pair', { code: $('pair-code').value, name: 'iPhone' });
    token = newToken;
    localStorage.setItem(TOKEN_KEY, token);
    $('pair-code').value = '';
    startDashboard();
  } catch (e) {
    err.textContent = e.message;
    err.hidden = false;
  }
}

async function startDashboard() {
  show('dashboard');
  await refresh();
}

// ---- boot ------------------------------------------------------------------

$('pair-form').addEventListener('submit', pair);
$('enable').addEventListener('click', enableNotifications);
$('test').addEventListener('click', sendTest);
$('unpair').addEventListener('click', unpair);
for (const kind of ['five_hour_reset', 'weekly_reset', 'account_switch']) {
  $(`pref-${kind}`).addEventListener('change', (e) => setPref(kind, e.target.checked));
}

if ('serviceWorker' in navigator) {
  navigator.serviceWorker.register('/sw.js', { scope: '/' }).then((r) => {
    registration = r;
  }, () => {});
}

if (isIOS && !isStandalone) show('install');
else if (!token) show('pair');
else startDashboard();

// Countdowns tick locally; the server is asked for fresh numbers every 30 s while visible.
setInterval(() => status && !document.hidden && renderStatus(), 1000);
setInterval(() => token && !document.hidden && refresh(), 30_000);
document.addEventListener('visibilitychange', () => !document.hidden && token && refresh());
