// The one Durable Object behind this deployment. It stores devices, the latest
// usage and the reset schedule, and fires reset notifications with a storage
// alarm at the exact reset time, laptop on or off.
//
// It never holds a Claude login: the laptop app uploads only percentages and
// reset timestamps (/ingest/*, bearer INGEST_TOKEN).

import { buildPushRequest, b64urlEncode } from './webpush.js';

const KINDS = {
  five_hour_reset: { window: 'five_hour', title: 'Claude: 5h limit reset', body: 'Full usage available.' },
  weekly_reset: { window: 'seven_day', title: 'Claude: weekly limit reset', body: 'Full usage available.' },
};
const WEEK = 7 * 86400;
// The browsers' push services: Apple (iPhone, iPad, Mac Safari), Google (Chrome, Edge, Samsung
// Internet and the rest on Android; older Chrome used android.googleapis.com), Mozilla (Firefox)
// and Microsoft (Edge on Windows).
const PUSH_HOSTS = ['web.push.apple.com', 'fcm.googleapis.com', 'android.googleapis.com', 'push.services.mozilla.com', 'notify.windows.com'];
const PAIR_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
const PAIR_TTL_MS = 24 * 3600_000;  // codes travel as links: people may not open them right away
const MAX_PENDING_PAIRINGS = 5;
const MAX_PAIR_FAILURES = 5;
const TEST_COOLDOWN_MS = 10_000;
const TEST_DELAY_S = 10;  // time to lock the phone and see the alert arrive as it would for real
const MAX_SENT = 1000;
const B64URL = /^[A-Za-z0-9_-]+={0,2}$/;
const LEVELS = [25, 50, 75, 85, 100];  // usage alerts (percent used), each on/off per phone
const REMINDERS = [60, 30, 15, 5];       // reset reminders (minutes before), each on/off per phone
const SWITCH_AT = 95;                    // where the Windows app switches by default
// Notifications are short: a title and one line with the numbers (the lock screen clips more).
const WINDOWS = [['five_hour', '5h'], ['seven_day', 'weekly']];
const ALERT_PREFS = [...LEVELS.map((l) => `usage_${l}`), ...REMINDERS.map((m) => `remind_${m}`)];
// Every kind of notification has its own switch on the phone (missing = on).
const OWNER_PREFS = ['heads_up', 'switch_alerts', 'cap_changes', 'reset_alerts', 'reserve_offer', 'reserve_asks', ...ALERT_PREFS];
const USER_PREFS = ['five_hour_reset', 'weekly_reset', 'account_switch', 'access_changes', 'reserve_released', ...ALERT_PREFS];
const HEADS_UP_COOLDOWN_MS = 10_000;
const RESERVE_ASK_COOLDOWN_MS = 5 * 60_000;  // asking a friend for their reserve: once every 5 minutes
const SWITCH_REQUEST_TTL_MS = 2 * 60_000;  // a switch asked from the phone, if the PC doesn't pick it up  // only stops an accidental double tap
// Owner devices belong to the friend whose Claude account is borrowed; they only ever
// see that one account. Every other device is the user's own.
const isOwner = (d) => d.role === 'owner';

function duration(seconds) {
  if (seconds <= 0) return 'now';
  const m = Math.floor(seconds / 60);
  if (m < 60) return `${Math.max(1, m)}m`;
  const h = Math.floor(m / 60);
  if (h < 24) return m % 60 ? `${h}h ${m % 60}m` : `${h}h`;
  return h % 24 ? `${Math.floor(h / 24)}d ${h % 24}h` : `${Math.floor(h / 24)}d`;
}

const cleanName = (s, fallback) => String(s || '').replace(/[^\w .'@-]/g, '').slice(0, 40) || fallback;
const enc = new TextEncoder();

const json = (data, status = 200) =>
  new Response(JSON.stringify(data), { status, headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' } });
const empty = (status) => new Response(null, { status });
const isInt = (v) => Number.isSafeInteger(v);
const USAGE_HISTORY_MS = 5 * 3600 * 1000;  // how far back a friend's usage graph goes
const isCounts = (v, max) => Array.isArray(v) && v.length <= max && v.every((n) => isInt(n) && n >= 0);
// The PC's live tokens: the pile written in this stretch of work (as Claude Code counts it), the
// speed now, each second of the last 5 minutes, each minute of the last 15.
const cleanTokens = (t) =>
  t && isInt(t.rate) && t.rate >= 0 && isCounts(t.history, 30) && (t.series === undefined || isCounts(t.series, 600)) &&
  (t.count === undefined || (isInt(t.count) && t.count >= 0)) && (t.pile === undefined || isInt(t.pile))
    ? { rate: t.rate, count: t.count || 0, working: t.working === true, pile: t.pile || 0, mode: t.mode === 'pile' ? 'pile' : 'speed',
        history: t.history, series: t.series || null,
        left: isInt(t.left) && t.left >= 0 ? t.left : null, at: Date.now() }  // left: the taskbar's tokens left
    : null;
const nowSeconds = () => Math.floor(Date.now() / 1000);

async function sha256Hex(text) {
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', enc.encode(String(text))));
  return [...digest].map((b) => b.toString(16).padStart(2, '0')).join('');
}

// Constant-time comparison via HMAC of both sides with a random key.
async function safeEqual(a, b) {
  const key = await crypto.subtle.generateKey({ name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  const [x, y] = await Promise.all([
    crypto.subtle.sign('HMAC', key, enc.encode(String(a))),
    crypto.subtle.sign('HMAC', key, enc.encode(String(b))),
  ]);
  const u = new Uint8Array(x);
  const v = new Uint8Array(y);
  let diff = 0;
  for (let i = 0; i < u.length; i++) diff |= u[i] ^ v[i];
  return diff === 0;
}

function randomToken(bytes = 32) {
  return b64urlEncode(crypto.getRandomValues(new Uint8Array(bytes)));
}

export function isAllowedEndpoint(endpoint, testOrigin) {
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

// "iPhone", "iPad", "Android", "Mac", "Windows" or "other": the health check's device column.
function deviceKind(ua) {
  const s = String(ua || '');
  if (/iPhone/.test(s)) return 'iPhone';
  if (/iPad/.test(s)) return 'iPad';
  if (/Android/.test(s)) return 'Android';
  if (/Macintosh/.test(s)) return 'Mac';
  if (/Windows/.test(s)) return 'Windows';
  return 'other';
}

function sanitizeSubscription(sub, testOrigin) {
  if (!sub || typeof sub !== 'object') return null;
  const { endpoint, keys } = sub;
  if (typeof endpoint !== 'string' || endpoint.length > 1024 || !isAllowedEndpoint(endpoint, testOrigin)) return null;
  if (!keys || typeof keys.p256dh !== 'string' || typeof keys.auth !== 'string') return null;
  if (keys.p256dh.length < 40 || keys.p256dh.length > 200 || !B64URL.test(keys.p256dh)) return null;
  if (keys.auth.length < 8 || keys.auth.length > 64 || !B64URL.test(keys.auth)) return null;
  return { endpoint, keys: { p256dh: keys.p256dh, auth: keys.auth } };
}

// {five_hour: {low, medium, high} | null, seven_day: ...}: percent x100 one prompt uses.
function normalizePromptCost(c) {
  if (c === null || c === undefined) return null;
  if (typeof c !== 'object') return undefined;
  const out = {};
  for (const key of ['five_hour', 'seven_day']) {
    const w = c[key];
    if (w === null || w === undefined) { out[key] = null; continue; }
    if (typeof w !== 'object') return undefined;
    out[key] = {};
    for (const effort of ['low', 'medium', 'high']) {
      if (!isInt(w[effort]) || w[effort] < 1 || w[effort] > 10_000) return undefined;
      out[key][effort] = w[effort] / 100;
    }
  }
  return out;
}

function normalizeWindow(w) {
  if (w === null || w === undefined) return null;
  if (typeof w !== 'object' || !isInt(w.utilization_x100) || !isInt(w.resets_at)) return undefined;
  if (w.utilization_x100 < 0 || w.utilization_x100 > 100_000 || w.resets_at < 0) return undefined;
  return { percent: w.utilization_x100 / 100, resetsAt: w.resets_at };
}

// What a phone may see of an account (never its email).
const publicAccount = (a) => ({
  name: a.name, active: a.active, five_hour: a.five_hour, seven_day: a.seven_day, prompt_cost: a.promptCost || null,
});

// The rule the laptop enforces: the owner's cap or the user's own cap, whichever is lower.
const lower = (a, b) => (a && b ? Math.min(a, b) : a || b || null);
// Each account's suggested 5-hour cap, worked out on its own: the most of each 5-hour session that
// still uses up exactly what's left of that account's week (or of the user's weekly cap on it) at
// its weekly reset, counting every 5-hour window until then, around the clock - the session running
// now (to its own reset, with what it has used so far) and the ones after it. A full 5-hour session
// costs a fixed share of that account's week (promptCost, learnt by the PC). Caps in the cap menu's
// 5% steps; null where it can't be worked out yet. The phone's "Suggested caps" shows the same.
function suggestedFiveHourCap(acc, weekCap, nowS) {
  const week = acc.seven_day;
  const five = acc.five_hour;
  const pc = acc.promptCost;
  if (!week || !(week.resetsAt > nowS) || !pc || !pc.five_hour || !pc.seven_day) return null;
  if (!(pc.five_hour.high > 0) || !(pc.seven_day.high > 0)) return null;
  const share = pc.seven_day.high / pc.five_hour.high;  // % of the week per 1% of a 5-hour session
  const left = Math.max(0, (weekCap || 100) - week.percent);
  const inSession = !!five && five.resetsAt > nowS;
  const from = inSession ? Math.min(five.resetsAt, week.resetsAt) : nowS;
  const sessions = (inSession ? 1 : 0) + Math.max(0, week.resetsAt - from) / (5 * 3600);
  const cap = (left / share + (inSession ? five.percent : 0)) / Math.max(1, sessions);
  return Math.min(100, Math.max(5, Math.floor(cap / 5) * 5));
}

function suggestedFiveHourCaps(accounts, weekCapOf, nowS) {
  const caps = {};
  for (const [email, acc] of Object.entries(accounts)) caps[email] = suggestedFiveHourCap(acc, weekCapOf(email), nowS);
  return { caps };
}

// A friend can free what a 5-hour cap holds back for them, until that 5-hour window resets.
const reserveOpen = (r) => !!(r && r.until > Date.now() / 1000);
function effectivePolicy(owner = {}, mine = {}, reserve = null) {
  return {
    capFiveHour: reserveOpen(reserve) ? null : lower(owner.capFiveHour, mine.capFiveHour),
    capWeekly: lower(owner.capWeekly, mine.capWeekly),
    allow: owner.allow !== false,
  };
}
const capValue = (v) => (v === null || v === undefined ? null : isInt(v) && v >= 5 && v <= 100 ? v : undefined);

export class Hub {
  constructor(state, env) {
    this.state = state;
    this.storage = state.storage;
    this.env = env;
    this.fetchImpl = (...args) => fetch(...args);
    this.pairAttempts = { windowStart: 0, count: 0 };
  }

  // ---- storage helpers --------------------------------------------------------

  async devices() {
    return (await this.storage.get('devices')) || [];
  }

  async saveDevices(devices) {
    await this.storage.put('devices', devices);
  }

  async schedule() {
    return (await this.storage.get('schedule')) || { five_hour_reset: 0, weekly_reset: 0 };
  }

  async sent() {
    return (await this.storage.get('sent')) || [];
  }

  async markSent(id) {
    const sent = await this.sent();
    sent.push({ id, at: Date.now() });
    await this.storage.put('sent', sent.slice(-MAX_SENT));
  }

  // Next alarm = earliest future reset we know about (the live account's and every account's).
  async rearm(schedule) {
    const now = nowSeconds();
    const accountResets = (await this.storage.get('accountResets')) || {};
    const tests = (await this.storage.get('pendingTests')) || [];
    const resets = [...Object.values(schedule), ...Object.values(accountResets)].filter((t) => t > 0);
    const reminders = resets.flatMap((t) => REMINDERS.map((m) => t - m * 60)).filter((t) => t > now);
    const times = [...resets, ...reminders, ...tests.map((x) => x.at)].filter((t) => t > 0);
    if (times.length) await this.storage.setAlarm(Math.min(...times) * 1000);
    else await this.storage.deleteAlarm();
  }

  // ---- account owners ----------------------------------------------------------------

  async notifyOwners(email, pref, note, id) {
    if (id) {
      if ((await this.sent()).some((e) => e.id === id)) return { duplicate: true };
      await this.markSent(id);
    }
    return this.sendToMany((d) => isOwner(d) && d.ownerEmail === email && d.prefs[pref] !== false, note, { topic: pref });
  }

  // Usage alerts at 25/50/75/85/100% (and an owner's cap): the user hears about every account,
  // an owner about theirs. Each phone hears once per window, about the highest level it has on.
  async usageAlerts(email, acc, pol, userName, now, mine) {
    const devices = await this.devices();
    for (const [key, label] of WINDOWS) {
      const w = acc[key];
      if (!w || !(w.resetsAt > 0)) continue;
      const window = Math.round(w.resetsAt / 60);
      const pct = Math.round(w.percent);
      const left = `${Math.max(0, 100 - pct)}% left · resets in ${duration(w.resetsAt - now)}`;
      const ownerCap = pol ? (key === 'five_hour' ? pol.capFiveHour : pol.capWeekly) : null;
      const myCap = mine ? (key === 'five_hour' ? mine.capFiveHour : mine.capWeekly) : null;
      for (const audience of ['user', 'owner']) {
        const cap = audience === 'owner' ? ownerCap : myCap;
        const levels = cap ? [...new Set([...LEVELS, cap])].sort((a, b) => a - b) : LEVELS;
        const crossed = levels.filter((l) => w.percent >= l);
        if (!crossed.length) continue;
        // One small record per audience, account and limit: the highest level already alerted
        // in the current window (the sent-id list is too short-lived for a 7-day window).
        const alerted = (await this.storage.get('alertLevels')) || {};
        const slot = `${audience}|${email}|${key}`;
        const done = alerted[slot] && alerted[slot].window === window ? alerted[slot].level : 0;
        const fresh = crossed.filter((l) => l > done);
        if (!fresh.length) continue;
        alerted[slot] = { window, level: crossed[crossed.length - 1] };
        await this.storage.put('alertLevels', alerted);
        const enabled = (d, l) => l === cap || d.prefs[`usage_${l}`] !== false;
        const groups = new Map();
        for (const d of devices) {
          if (!d.subscription || (audience === 'owner' ? !(isOwner(d) && d.ownerEmail === email) : isOwner(d))) continue;
          const top = [...crossed].reverse().find((l) => enabled(d, l));
          if (top !== undefined && fresh.includes(top)) groups.set(top, [...(groups.get(top) || []), d.id]);
        }
        for (const [top, ids] of groups) {
          const title = top >= 100 ? `${acc.name}: ${label} limit reached`
            : cap && top === cap ? `${acc.name}: ${cap}% ${label} cap hit`
            : `${acc.name}: ${pct}% of ${label} used`;
          const who = acc.active ? (audience === 'owner' ? ` · ${userName} on it` : ' · in use') : '';
          const to = new Set(ids);
          await this.sendToMany((d) => to.has(d.id), { title, body: left + who, tag: `usage_${key}_${email}` }, { topic: `usage_${key}` });
        }
      }
    }
  }

  // Every account's usage as the laptop sees it right now. Notifies each account's owner
  // when the user starts/stops using it and as its usage crosses 50/75/90/100% or the
  // owner's cap, schedules every account's reset alerts, and answers with the owners' rules.
  // A friend sees only their own account: the PC's token speed while their account is the one in
  // use (0 otherwise), and a graph of only the times it was in use (flat the rest of the time).
  async tokensForOwner(device) {
    const email = device.ownerEmail;
    const acc = ((await this.storage.get('accounts')) || {})[email];
    if (!acc) return null;
    const t = await this.latestTokens();
    const since = ((await this.storage.get('activeSince')) || {})[email] || 0;
    const spans = [...(((await this.storage.get('activeSpans')) || {})[email] || [])];
    if (acc.active && since) spans.push([since, Infinity]);
    const theirs = (ms) => spans.some(([a, b]) => ms >= a && ms <= b);
    const at = t ? t.at : Date.now();
    const series = t && Array.isArray(t.series)
      ? t.series.map((v, i) => (theirs(at - (t.series.length - 1 - i) * 1000) ? v : 0))
      : Array(300).fill(0);
    return { rate: acc.active && t ? t.rate : 0, count: 0, working: false, pile: 0, mode: 'speed', history: [], series,
      left: acc.active && t && t.left != null ? t.left : null, at };
  }

  // The newer of the live copy and the stored one.
  async latestTokens() {
    const stored = (await this.storage.get('tokens')) || null;
    const live = this.liveTokens;
    return live && (!stored || live.at >= stored.at) ? live : stored;
  }

  async ingestAccounts(body) {
    if (!Array.isArray(body.accounts) || body.accounts.length > 10) return json({ error: 'invalid accounts' }, 400);
    const userName = cleanName(body.user_name, 'Your friend');
    const now = nowSeconds();
    const next = {};
    for (const a of body.accounts) {
      if (!a || typeof a.email !== 'string' || a.email.length > 200 || !a.email.includes('@')) return json({ error: 'invalid account' }, 400);
      const five = normalizeWindow(a.five_hour);
      const week = normalizeWindow(a.seven_day);
      const promptCost = normalizePromptCost(a.prompt_cost);
      if (five === undefined || week === undefined || promptCost === undefined || typeof a.active !== 'boolean') {
        return json({ error: 'invalid account' }, 400);
      }
      next[a.email.toLowerCase()] = { name: cleanName(a.name, a.email.split('@')[0]), active: a.active, five_hour: five, seven_day: week, promptCost };
    }
    const prev = (await this.storage.get('accounts')) || {};
    const firstUpload = Object.keys(prev).length === 0;
    await this.storage.delete(['ownerResets', 'pairing']);  // left over from earlier versions
    await this.storage.put('accounts', next);
    // When each account came into use, and the stretches each was in use lately: a friend's live
    // graph shows only their own account's share of the PC's tokens.
    const activeSince = (await this.storage.get('activeSince')) || {};
    const activeSpans = (await this.storage.get('activeSpans')) || {};
    for (const [email, a] of Object.entries(next)) {
      if (!a.active) {
        if (activeSince[email]) (activeSpans[email] = activeSpans[email] || []).push([activeSince[email], Date.now()]);
        delete activeSince[email];
      } else if (!activeSince[email]) {
        activeSince[email] = Date.now();
      }
    }
    for (const email of Object.keys(activeSpans)) {
      activeSpans[email] = activeSpans[email].filter(([, end]) => Date.now() - end < USAGE_HISTORY_MS);
      if (!activeSpans[email].length) delete activeSpans[email];
    }
    await this.storage.put('activeSince', activeSince);
    await this.storage.put('activeSpans', activeSpans);
    // Each account's 5-hour usage over time (a point a minute, or on each change), for a friend's
    // phone to graph their own usage whoever is using the account.
    const history = (await this.storage.get('usageHistory')) || {};
    const nowMs = Date.now();
    for (const email of Object.keys(history)) if (!next[email]) delete history[email];
    for (const [email, a] of Object.entries(next)) {
      if (!a.five_hour) continue;
      const pct = a.five_hour.resetsAt * 1000 <= nowMs ? 0 : Math.round(a.five_hour.percent * 10) / 10;
      const list = (history[email] || []).filter(([t]) => nowMs - t <= USAGE_HISTORY_MS);
      const last = list[list.length - 1];
      if (!last || last[1] !== pct || nowMs - last[0] >= 60_000) list.push([nowMs, pct]);
      history[email] = list;
    }
    await this.storage.put('usageHistory', history);
    await this.storage.put('accountsReceivedAt', Date.now());
    await this.storage.put('userName', userName);
    // Anything new in the account in use's usage response (limit resets, someday): kept per account
    // until that account's next reading says otherwise. The phones show a box only while there's one.
    const ex = body.extras;
    if (ex && typeof ex.email === 'string' && Array.isArray(ex.items) && next[ex.email.toLowerCase()]) {
      const extras = (await this.storage.get('usageExtras')) || {};
      const items = ex.items.slice(0, 5).filter((x) => x && typeof x.key === 'string' && /^\w{1,63}$/.test(x.key))
        .map((x) => ({ key: x.key, endsAt: Number.isFinite(x.ends_at) && x.ends_at > 0 ? x.ends_at : null }));
      if (items.length) extras[ex.email.toLowerCase()] = items;
      else delete extras[ex.email.toLowerCase()];
      await this.storage.put('usageExtras', extras);
    }
    // Limit resets (one-off resets Anthropic grants an account), read for every account by the PC
    // about every 30 minutes; an account missing from a read keeps what was last seen. Shown only.
    if (body.resets && Array.isArray(body.resets.accounts)) {
      const resets = (await this.storage.get('resetGrants')) || {};
      for (const r of body.resets.accounts.slice(0, 10)) {
        if (!r || typeof r.email !== 'string' || !next[r.email.toLowerCase()]) continue;
        resets[r.email.toLowerCase()] = {
          eligible: !!r.eligible,
          reason: typeof r.reason === 'string' ? r.reason.slice(0, 40) : '',
          grants: (Array.isArray(r.grants) ? r.grants : []).slice(0, 5).map((g) => ({
            label: typeof g.label === 'string' ? g.label.slice(0, 120) : '',
            left: Number.isFinite(g.left) ? g.left : 0,
            total: Number.isFinite(g.total) ? g.total : 0,
            endsAt: Number.isFinite(g.ends_at) && g.ends_at > 0 ? g.ends_at : null,
          })),
        };
      }
      await this.storage.put('resetGrants', resets);
    }
    // When the PC last checked usage with Anthropic and when it checks next: the phones' countdown.
    const check = body.usage_check;
    if (check && Number.isFinite(check.at) && Number.isFinite(check.next) && check.next > 0) {
      await this.storage.put('usageCheck', { at: check.at, next: check.next });
    }
    // The live token speed on the PC (see cleanTokens).
    const tokens = cleanTokens(body.tokens);
    if (tokens) await this.storage.put('tokens', tokens);

    const devices = await this.devices();
    const owned = new Set(devices.filter(isOwner).map((d) => d.ownerEmail));
    // Once: friends' accounts paired more than once keep only the phone used most recently.
    if (!(await this.storage.get('ownersPruned'))) {
      const all = await this.devices();
      const latest = {};
      for (const d of all.filter(isOwner)) {
        const t = d.seenAt || Date.parse(d.createdAt) || 0;
        if (!latest[d.ownerEmail] || t >= latest[d.ownerEmail].t) latest[d.ownerEmail] = { id: d.id, t };
      }
      await this.saveDevices(all.filter((d) => !isOwner(d) || latest[d.ownerEmail].id === d.id));
      await this.storage.put('ownersPruned', true);
    }
    const policies = (await this.storage.get('policies')) || {};
    const myCaps = (await this.storage.get('myCaps')) || {};
    // Automatic caps: the user's 5-hour cap follows the suggested one. Changed quietly - no one is
    // notified about these, only about caps set by hand.
    const autoCaps = (await this.storage.get('autoCaps')) || {};
    let autoChanged = false;
    const suggested = suggestedFiveHourCaps(next, (e) => lower((policies[e] || {}).capWeekly, (myCaps[e] || {}).capWeekly), now);
    for (const [email, auto] of Object.entries(autoCaps)) {
      if (!next[email] || !auto) continue;
      const mine = myCaps[email] || {};
      const cap = suggested.caps[email];
      if (cap && cap !== mine.capFiveHour) {
        myCaps[email] = { capFiveHour: cap, capWeekly: mine.capWeekly || null };
        autoChanged = true;
      }
    }
    if (autoChanged) await this.storage.put('myCaps', myCaps);
    // An owner's automatic 5-hour cap on their own account: the same suggestion, just as quietly.
    const ownerAutoCaps = (await this.storage.get('ownerAutoCaps')) || {};
    const noAuto = (await this.storage.get('noAuto')) || {};
    let ownerAutoChanged = false;
    for (const [email, auto] of Object.entries(ownerAutoCaps)) {
      const cap = next[email] && auto ? suggested.caps[email] : null;
      if (cap && cap !== (policies[email] || {}).capFiveHour) {
        policies[email] = { capWeekly: null, allow: true, ...policies[email], capFiveHour: cap };
        ownerAutoChanged = true;
      }
    }
    if (ownerAutoChanged) await this.storage.put('policies', policies);
    const reserves = (await this.storage.get('reserves')) || {};
    for (const e of Object.keys(reserves)) if (!reserveOpen(reserves[e])) delete reserves[e];  // ended at the reset
    await this.storage.put('reserves', reserves);
    const rules = Object.fromEntries(Object.keys(next).map((e) => [e, effectivePolicy(policies[e], myCaps[e], reserves[e])]));
    const accountResets = (await this.storage.get('accountResets')) || {};
    const minute = Math.floor(Date.now() / 60000);

    for (const [email, acc] of Object.entries(next)) {
      // Every account's resets are announced: to its owner and to the user.
      for (const [key] of WINDOWS) {
        const w = acc[key];
        if (w && w.resetsAt > 0) accountResets[`${email}|${key}`] = w.resetsAt > now ? w.resetsAt : accountResets[`${email}|${key}`] || 0;
      }
      if (!owned.has(email)) continue;
      const usageLine = WINDOWS.map(([k, label]) => (acc[k] ? `${label} ${Math.round(acc[k].percent)}%` : null)).filter(Boolean).join(' · ');

      // Started / stopped using the owner's account.
      const was = prev[email] && prev[email].active;
      if (!firstUpload && acc.active !== !!was) {
        // Leaving: what's left is theirs again, until the user comes back.
        const left = (w) => (w ? Math.max(0, 100 - Math.round(w.percent)) : null);
        const leftBody = acc.five_hour
          ? `${left(acc.five_hour)}% of your 5h left for you${acc.seven_day ? ` (weekly ${left(acc.seven_day)}%)` : ''}. ${userName} is inactive now.`
          : `${userName} is inactive now.`;
        await this.notifyOwners(email, 'switch_alerts', {
          title: acc.active ? `${userName} is now on your account` : `${userName} left your account`,
          body: acc.active ? `Used: ${usageLine}` : leftBody,
          tag: 'owner_switch',
        }, `owner-switch:${email}:${acc.active ? 'on' : 'off'}:${minute}`);
      }

    }
    for (const [email, acc] of Object.entries(next)) {
      // An automatic 5-hour cap moves, so it isn't an alert level of its own (the usual ones are).
      const mine = autoCaps[email] && myCaps[email] ? { ...myCaps[email], capFiveHour: null } : myCaps[email];
      const pol = ownerAutoCaps[email] && policies[email] ? { ...policies[email], capFiveHour: null } : policies[email];
      await this.usageAlerts(email, acc, pol, userName, now, mine);
    }

    // (No automatic "switching to you soon": the user tells an owner from the phone when they want.)
    for (const slot of Object.keys(accountResets)) if (!next[slot.split('|')[0]]) delete accountResets[slot];
    for (const name of ['alertLevels', 'reminded']) {  // forget accounts no longer on the PC
      const map = (await this.storage.get(name)) || {};
      const keep = Object.fromEntries(Object.entries(map).filter(([slot]) => next[slot.split('|').at(-2)] || next[slot.split('|')[0]]));
      if (Object.keys(keep).length !== Object.keys(map).length) await this.storage.put(name, keep);
    }
    await this.storage.put('accountResets', accountResets);
    await this.rearm(await this.schedule());

    return json({
      policies: Object.keys(next).map((email) => {
        const p = policies[email] || {};
        const r = rules[email];
        return {
          email, cap_five_hour: r.capFiveHour, cap_weekly: r.capWeekly, allow: r.allow,
          owner_cap_five_hour: p.capFiveHour || null, owner_cap_weekly: p.capWeekly || null, updated_at: p.updatedAt || 0,
          // Freed by the owner: the 5-hour bar shows green from this cap to 100%.
          reserve_from: reserveOpen(reserves[email]) ? reserves[email].cap : null,
          auto_five_hour: !!autoCaps[email],  // the 5-hour cap moves by itself: its changes aren't news
          owner_auto_five_hour: !!ownerAutoCaps[email],  // the owner's does
          no_auto: !!noAuto[email],  // the user switches to it by hand only
        };
      }),
    });
  }

  vapid() {
    return {
      publicKey: this.env.VAPID_PUBLIC_KEY,
      privateKey: this.env.VAPID_PRIVATE_KEY,
      subject: this.env.VAPID_SUBJECT || 'mailto:claude-usage-monitor@users.noreply.github.com',
    };
  }

  // ---- push delivery --------------------------------------------------------------

  // Sends to one device; drops subscriptions the push service reports gone.
  async sendTo(device, note, options = {}) {
    if (!device.subscription) return { ok: false };
    // d + r: the phone reports back when the push arrives and when it's shown (/api/receipt), so the
    // health check can tell "Apple took it" from "the phone got it" from "iOS showed it".
    const rid = b64urlEncode(crypto.getRandomValues(new Uint8Array(6)));
    const payload = JSON.stringify({ title: note.title, body: note.body, url: note.url || '/', timestamp: Date.now(), d: device.id, r: rid });
    const sent = (r) => ({ ...r, rid });
    return sent(await this.pushOnce(device, payload, options));
  }

  async pushOnce(device, payload, options) {
    for (let attempt = 1; attempt <= 2; attempt++) {
      try {
        // No Topic: Apple would fold pushes of one kind into the latest while a phone is offline (and
        // treat it as a quiet update of the one shown). Each notification goes through on its own.
        const req = await buildPushRequest(device.subscription, payload, this.vapid(), { ...options, topic: undefined });
        const res = await this.fetchImpl(req.url, { method: req.method, headers: req.headers, body: req.body });
        if (res.status >= 200 && res.status < 300) return { ok: true, status: res.status };
        if (res.status === 404 || res.status === 410) return { ok: false, gone: true, status: res.status };
        if (res.status < 500 && res.status !== 429) return { ok: false, status: res.status };
      } catch (err) {
        if (attempt === 2) return { ok: false, error: String(err && err.message) };
      }
    }
    return { ok: false };
  }

  async sendToMany(filter, note, options) {
    const devices = await this.devices();
    const targets = devices.filter((d) => d.subscription && filter(d));
    const results = await Promise.all(targets.map((d) => this.sendTo(d, note, options)));
    // Each phone keeps how its last push went (the health check lists it), and a phone the push
    // service says is gone stops being sent to.
    // Read fresh after the pushes: a phone may have subscribed again meanwhile, and that must stay.
    if (targets.length) {
      const fresh = await this.devices();
      targets.forEach((t, i) => {
        const d = fresh.find((x) => x.id === t.id);
        if (!d) return;
        const r = results[i];
        d.lastPush = { at: Date.now(), status: r.status || (r.error ? 'error' : 'none') };
        d.pushes = [...(d.pushes || []), { rid: r.rid, at: Date.now(), title: note.title, status: d.lastPush.status }].slice(-5);
        if (r.gone && d.subscription && d.subscription.endpoint === t.subscription.endpoint) d.subscription = null;
      });
      await this.saveDevices(fresh);
    }
    const delivered = results.filter((r) => r.ok).length;
    if (targets.length) {
      // What went out, to whom: `status.ps1 -Log` shows it, to check every number against the PC.
      const to = [...new Set(targets.map((d) => (isOwner(d) ? d.ownerName || 'owner' : 'you')))].join(', ');
      const log = (await this.storage.get('notifyLog')) || [];
      const failed = results.filter((r) => !r.ok).map((r) => r.status || (r.gone ? 'gone' : r.error || 'error'));
      log.push({ at: Date.now(), to, title: note.title, body: note.body, delivered, of: targets.length, ...(failed.length ? { failed } : {}) });
      await this.storage.put('notifyLog', log.slice(-100));
    }
    return { delivered, targets: targets.length };
  }

  // "bob 5h 95% · wk 52%  |  alice 5h 16% · wk 21%": what the cloud has right now, for the
  // test notification (a phone's own accounts only).
  async usageSummary(device) {
    const accounts = (await this.storage.get('accounts')) || {};
    const fmt = (name, a) => `${name} 5h ${a.five_hour ? Math.round(a.five_hour.percent) : '--'}% · wk ${a.seven_day ? Math.round(a.seven_day.percent) : '--'}%`;
    if (isOwner(device)) {
      const a = accounts[device.ownerEmail];
      return a ? `Now: ${fmt(a.name, a)}` : 'Notifications are working.';
    }
    const list = Object.values(accounts);
    if (list.length) return `Now: ${list.map((a) => fmt(a.name, a)).join('  |  ')}`;
    const u = (await this.storage.get('usage')) || null;
    return u ? `Now: ${fmt('Claude', u)}` : 'Notifications are working.';
  }

  async notifyReset(kind, resetAt) {
    const id = `${kind}:${resetAt}`;
    if ((await this.sent()).some((e) => e.id === id)) return { duplicate: true };
    // With several accounts, every account's reset is announced by name instead (alarm).
    if (Object.keys((await this.storage.get('accounts')) || {}).length) return { delivered: 0, targets: 0, perAccount: true };
    await this.markSent(id); // first: a retry can never double-notify
    const { title, body } = KINDS[kind];
    const result = await this.sendToMany((d) => !isOwner(d) && d.prefs[kind] !== false, { title, body, tag: kind }, { topic: kind });
    console.log(`reset ${id}: notified ${result.delivered}/${result.targets}`);
    return result;
  }

  // ---- alarm: fire due resets ---------------------------------------------------

  async alarm() {
    const schedule = await this.schedule();
    const now = nowSeconds();

    // Test notifications asked for a few seconds ago.
    const tests = (await this.storage.get('pendingTests')) || [];
    const due = tests.filter((x) => x.at <= now + 1);
    if (due.length) {
      await this.storage.put('pendingTests', tests.filter((x) => x.at > now + 1));
      for (const d of (await this.devices()).filter((x) => due.some((t) => t.deviceId === x.id))) {
        await this.sendToMany((x) => x.id === d.id, { title: 'Claude Usage test', body: await this.usageSummary(d), tag: 'test' }, { ttl: 600 });
      }
    }
    for (const kind of Object.keys(KINDS)) {
      const t = schedule[kind];
      if (t > 0 && t <= now + 1) {
        await this.notifyReset(kind, t);
        // The weekly window rolls on a fixed schedule even if the laptop stays offline.
        // A new 5-hour window only starts with use, so wait for the laptop to report it.
        schedule[kind] = kind === 'weekly_reset' ? t + WEEK : 0;
      }
    }
    await this.storage.put('schedule', schedule);

    // Every account's resets: its owner hears about their own, the user about all of them.
    const accountResets = (await this.storage.get('accountResets')) || {};
    const accounts = (await this.storage.get('accounts')) || {};
    for (const [slot, t] of Object.entries(accountResets)) {
      if (!(t > 0 && t <= now + 1)) continue;
      const [email, key] = slot.split('|');
      const label = key === 'five_hour' ? '5h' : 'weekly';
      const name = (accounts[email] && accounts[email].name) || email.split('@')[0];
      await this.notifyOwners(email, 'reset_alerts', {
        title: `${name}: ${label} limit reset`,
        body: 'Full usage available.',
        tag: `owner_reset_${key}`,
      }, `owner-reset:${email}:${key}:${Math.round(t / 60)}`);
      const id = `account-reset:${email}:${key}:${Math.round(t / 60)}`;
      if (!(await this.sent()).some((e) => e.id === id)) {
        await this.markSent(id);
        const kind = key === 'five_hour' ? 'five_hour_reset' : 'weekly_reset';
        await this.sendToMany((d) => !isOwner(d) && d.prefs[kind] !== false, {
          title: `${name}: ${label} limit reset`,
          body: 'Full usage available.',
          tag: `${kind}_${email}`,
        }, { topic: kind });
      }
      accountResets[slot] = key === 'seven_day' ? t + WEEK : 0;
    }
    await this.storage.put('accountResets', accountResets);
    await this.resetReminders(now);
    await this.rearm(schedule);
  }

  // "alice: 5-hour limit resets in 15m": 1 h, 30, 15 and 5 minutes before each reset (each on/off
  // per phone). A reset learnt late gets only the nearest reminder. Unused windows are skipped.
  async resetReminders(now) {
    const accounts = (await this.storage.get('accounts')) || {};
    const usage = (await this.storage.get('usage')) || {};
    const slots = Object.keys(accounts).length
      ? Object.entries((await this.storage.get('accountResets')) || {}).map(([slot, t]) => {
        const [email, key] = slot.split('|');
        return { email, key, t, acc: accounts[email] };
      })
      : Object.entries(await this.schedule()).map(([kind, t]) => ({ email: 'live', key: KINDS[kind].window, t, acc: null }));
    for (const { email, key, t, acc } of slots) {
      if (!(t > now + 1)) continue;
      const due = REMINDERS.filter((m) => t - m * 60 <= now + 1);
      if (!due.length) continue;
      const m = Math.min(...due);
      // One record per account and limit: the nearest reminder already sent for this reset.
      const reminded = (await this.storage.get('reminded')) || {};
      const slot = `${email}|${key}`;
      if (reminded[slot] && reminded[slot].t === t && reminded[slot].lead <= m) continue;
      reminded[slot] = { t, lead: m };
      await this.storage.put('reminded', reminded);
      const w = acc ? acc[key] : usage[key];
      if (w && w.percent < 1) continue;
      const label = key === 'five_hour' ? '5h' : 'weekly';
      const name = acc ? acc.name : 'Claude';
      const kind = key === 'five_hour' ? 'five_hour_reset' : 'weekly_reset';
      const pref = `remind_${m}`;
      const used = w ? `${Math.round(w.percent)}% used now` : 'Full usage soon';
      await this.sendToMany((d) => !isOwner(d) && d.prefs[kind] !== false && d.prefs[pref] !== false, {
        title: `${name}: ${label} resets in ${duration(t - now)}`,
        body: used,
        tag: `remind_${key}_${email}`,
      }, { topic: 'reminder' });
      if (acc) {
        await this.sendToMany((d) => isOwner(d) && d.ownerEmail === email && d.prefs.reset_alerts !== false && d.prefs[pref] !== false, {
          title: `${name}: ${label} resets in ${duration(t - now)}`,
          body: used,
          tag: `remind_${key}`,
        }, { topic: 'reminder' });
      }
    }
    await this.reserveOffers(now);
  }

  // "Your 5-hour limit resets in 30m. Let Sam use your 40% until then?": half an hour before a
  // friend's 5-hour reset, while a cap holds some of it back for them (once per reset; on/off).
  async reserveOffers(now) {
    const accounts = (await this.storage.get('accounts')) || {};
    const policies = (await this.storage.get('policies')) || {};
    const myCaps = (await this.storage.get('myCaps')) || {};
    const reserves = (await this.storage.get('reserves')) || {};
    const userName = (await this.storage.get('userName')) || 'Your friend';
    const owned = new Set((await this.devices()).filter(isOwner).map((d) => d.ownerEmail));
    for (const [email, acc] of Object.entries(accounts)) {
      const w = acc.five_hour;
      const cap = lower((policies[email] || {}).capFiveHour, (myCaps[email] || {}).capFiveHour);
      if (!owned.has(email) || !cap || !w || reserveOpen(reserves[email])) continue;
      const left = w.resetsAt - now;
      const free = 100 - Math.max(Math.round(w.percent), cap);
      if (left <= 60 || left > 30 * 60 + 5 || free < 5) continue;  // (the alarm may fire a moment early)
      await this.notifyOwners(email, 'reserve_offer', {
        title: `Your 5-hour limit resets in ${duration(left)}`,
        body: `Let ${userName} use your ${free}% until then? Tap to choose.`,
        tag: 'reserve_offer', url: '/?reserve',
      }, `reserve-offer:${email}:${w.resetsAt}`);
    }
  }

  // What a friend's page shows about their reserve: what the cap keeps for them, and whether
  // they have freed it.
  async reserveFor(email) {
    const acc = ((await this.storage.get('accounts')) || {})[email];
    const p = ((await this.storage.get('policies')) || {})[email] || {};
    const mine = ((await this.storage.get('myCaps')) || {})[email] || {};
    const r = ((await this.storage.get('reserves')) || {})[email];
    const w = acc && acc.five_hour;
    const cap = lower(p.capFiveHour, mine.capFiveHour);
    if (!cap || !w || !(w.resetsAt > Date.now() / 1000)) return null;
    const open = reserveOpen(r);
    return { cap: open ? r.cap : cap, released: open, until: w.resetsAt, free: 100 - Math.max(Math.round(w.percent), open ? r.cap : cap) };
  }

  // ---- HTTP ----------------------------------------------------------------------

  async fetch(request) {
    const url = new URL(request.url);
    try {
      if (url.pathname.startsWith('/ingest/')) return await this.handleIngest(request, url.pathname);
      if (url.pathname.startsWith('/api/')) return await this.handleApi(request, url.pathname);
      return json({ error: 'Not found.' }, 404);
    } catch (err) {
      if (err instanceof SyntaxError) return json({ error: 'Invalid request body.' }, 400);
      console.error(err && err.stack);
      return json({ error: 'Server error.' }, 500);
    }
  }

  async readJson(request, limit = 16 * 1024) {
    const text = await request.text();
    if (text.length > limit) throw new SyntaxError('too large');
    return text ? JSON.parse(text) : {};
  }

  // ---- /ingest: the laptop app (and the setup scripts) ---------------------------

  async handleIngest(request, path) {
    const m = /^Bearer (\S{32,200})$/.exec(request.headers.get('Authorization') || '');
    if (!this.env.INGEST_TOKEN || !m || !(await safeEqual(m[1], this.env.INGEST_TOKEN))) return empty(401);

    // The PC owner's diagnostic: a test straight to one friend's phone (by account name), with the
    // push service's full answer (status, its reason, Apple's message id), not just "accepted".
    if (path === '/ingest/test-push' && request.method === 'POST') {
      const body = await this.readJson(request);
      const devices = await this.devices();
      const ids = new Set(devices.filter((d) => d.subscription && (body.for === 'you' ? !isOwner(d) : isOwner(d) && d.ownerName === body.for)).map((d) => d.id));
      const userName = (await this.storage.get('userName')) || 'your friend';
      const result = await this.sendToMany((d) => ids.has(d.id), { title: `Test from ${userName}`, body: 'If you see this, notifications work on this iPhone.' }, { ttl: 3600 });
      return json({ for: body.for, ...result });
    }

    if (path === '/ingest/health' && request.method === 'GET') {
      const devices = await this.devices();
      const pairings = (await this.storage.get('pairings')) || {};
      return json({
        ok: true,
        devices: devices.length,
        subscribed: devices.filter((d) => d.subscription).length,
        schedule: await this.schedule(),
        usageReceivedAt: (await this.storage.get('usageReceivedAt')) || 0,
        // Who is paired and which links are still open (no tokens, codes or emails).
        phones: devices.map((d) => ({
          id: d.id, for: isOwner(d) ? d.ownerName : 'you', pairedAt: d.createdAt, notifications: !!d.subscription,
          // Which push service (Apple's for an iPhone), what kind of device, and its last push.
          push: d.subscription ? new URL(d.subscription.endpoint).host : null, agent: d.agent || null,
          lastPush: d.lastPush || null, seenAt: d.seenAt || null, pushes: d.pushes || [], diag: d.diag || null,
        })),
        links: (pairings.codes || []).map((p) => ({ for: p.ownerName || 'you', expiresAt: p.expiresAt })),
        wrongCodeAttempts: pairings.failures || 0,
        log: ((await this.storage.get('notifyLog')) || []).slice(-40),
        // Every account's next resets (the schedule above is the single-account one).
        resets: await (async () => {
          const accounts = (await this.storage.get('accounts')) || {};
          return Object.entries((await this.storage.get('accountResets')) || {}).filter(([, t]) => t > 0).map(([slot, t]) => {
            const [email, key] = slot.split('|');
            return { for: (accounts[email] && accounts[email].name) || '?', limit: key === 'five_hour' ? '5h' : 'weekly', at: t };
          });
        })(),
      });
    }
    if (request.method !== 'POST') return empty(405);
    const body = await this.readJson(request, 8 * 1024);

    if (path === '/ingest/usage') {
      const plan = typeof body.plan === 'string' && /^[\w-]{0,32}$/.test(body.plan) ? body.plan : null;
      const windows = {};
      for (const key of ['five_hour', 'seven_day', 'seven_day_sonnet', 'seven_day_opus']) {
        const w = normalizeWindow(body[key]);
        if (w === undefined) return json({ error: `invalid ${key}` }, 400);
        windows[key] = w;
      }
      if (!isInt(body.fetched_at) || plan === null) return json({ error: 'invalid usage' }, 400);
      await this.storage.put('usage', { plan, fetchedAt: body.fetched_at, ...windows });
      await this.storage.put('usageReceivedAt', Date.now());
      // The laptop stopped sending accounts (claude-swap gone): back to single-account alerts.
      const accountsAt = (await this.storage.get('accountsReceivedAt')) || 0;
      if (accountsAt && Date.now() - accountsAt > 3600_000) {
        await this.storage.delete('accounts');
        await this.storage.delete('accountResets');
        await this.storage.delete('accountsReceivedAt');
      }

      // Track upcoming resets. Only future times are scheduled: a reset that already
      // passed was either notified by the alarm or happened while nobody could see it.
      const schedule = await this.schedule();
      const now = nowSeconds();
      for (const [kind, { window }] of Object.entries(KINDS)) {
        const w = windows[window];
        if (w === null) continue; // limit not reported: keep what we have
        if (w.resetsAt > now) schedule[kind] = w.resetsAt;
        else if (kind === 'five_hour_reset' && w.resetsAt === 0) schedule[kind] = 0; // no window running
        // otherwise keep the existing entry (a pending alarm, or the rolled-forward weekly reset)
      }
      await this.storage.put('schedule', schedule);
      await this.rearm(schedule);
      if (!Object.keys((await this.storage.get('accounts')) || {}).length) {
        await this.usageAlerts('live', { name: 'Claude', active: true, five_hour: windows.five_hour, seven_day: windows.seven_day }, null, '', now);
      }
      return empty(204);
    }

    if (path === '/ingest/event') {
      const { id, kind, reset_at: resetAt } = body;
      const now = nowSeconds();
      if (!KINDS[kind] || !isInt(resetAt) || resetAt < now - 30 * 86400 || resetAt > now + 86400 || id !== `${kind}:${resetAt}`) {
        return json({ error: 'invalid event' }, 400);
      }
      const result = await this.notifyReset(kind, resetAt);
      return json(result, result.duplicate ? 200 : 202);
    }

    if (path === '/ingest/notice') {
      const { id, title, body: text } = body;
      if (typeof id !== 'string' || !/^[\w:.>-]{1,100}$/.test(id) || typeof title !== 'string' || !title ||
          title.length > 80 || typeof text !== 'string' || text.length > 240) {
        return json({ error: 'invalid notice' }, 400);
      }
      const key = `notice:${id}`;
      if ((await this.sent()).some((e) => e.id === key)) return json({ duplicate: true });
      await this.markSent(key);
      const result = await this.sendToMany((d) => !isOwner(d) && d.prefs.account_switch !== false, { title, body: text, tag: 'account_switch' }, { topic: 'account_switch' });
      return json(result, 202);
    }

    if (path === '/ingest/accounts') return this.ingestAccounts(body);

    // Every 2 s while Claude Code is writing: kept in memory only (no storage write each time),
    // the accounts upload stores it every 15-60 s anyway.
    if (path === '/ingest/tokens') {
      const tokens = cleanTokens(body);
      if (!tokens) return json({ error: 'Bad token speed.' }, 400);
      this.liveTokens = tokens;
      return json({ ok: true });
    }

    // The PC asks every few seconds whether the phone wants it to switch accounts. A request is
    // handed over once, and only while fresh: a PC that was asleep doesn't switch later.
    if (path === '/ingest/commands') {
      const out = {};
      const request = await this.storage.get('switchRequest');
      if (request) {
        await this.storage.delete('switchRequest');
        if (Date.now() - request.at < SWITCH_REQUEST_TTL_MS) out.switch_to = request.email;
      }
      const refresh = await this.storage.get('refreshRequest');
      if (refresh) {
        await this.storage.delete('refreshRequest');
        if (Date.now() - refresh < SWITCH_REQUEST_TTL_MS) out.refresh = true;
      }
      return json(out);
    }

    if (path === '/ingest/unpair') {  // remove a paired phone/browser by id (status.ps1 lists them)
      const devices = await this.devices();
      const left = devices.filter((d) => d.id !== body.id);
      if (left.length === devices.length) return json({ error: 'No such device.' }, 404);
      await this.saveDevices(left);
      return json({ removed: 1 });
    }

    if (path === '/ingest/pair') {
      // Optional `owner`: an account's name or email, to pair that account owner's phone.
      let ownerEmail = null;
      let ownerName = null;
      if (body.owner) {
        const wanted = String(body.owner).toLowerCase();
        const accounts = (await this.storage.get('accounts')) || {};
        for (const [email, acc] of Object.entries(accounts)) {
          if (email === wanted || acc.name.toLowerCase() === wanted) {
            ownerEmail = email;
            ownerName = acc.name;
          }
        }
        if (!ownerEmail) return json({ error: `No account named ${body.owner} yet. Is the Windows app running with it saved?` }, 404);
      }
      const bytes = crypto.getRandomValues(new Uint8Array(8));
      let code = '';
      for (const b of bytes) code += PAIR_ALPHABET[b % PAIR_ALPHABET.length];
      const ttl = this.env.PAIR_TTL_MS ? Number(this.env.PAIR_TTL_MS) : PAIR_TTL_MS;
      const expiresAt = Date.now() + ttl;
      // Several codes may be pending (your phone, alice's, dave's); a new code for the same
      // person replaces their old one.
      const pending = (await this.pendingPairings()).filter((p) => (p.ownerEmail || null) !== ownerEmail);
      pending.push({ codeHash: await sha256Hex(code), expiresAt, ownerEmail, ownerName });
      const before = (await this.storage.get('pairings')) || {};
      await this.storage.put('pairings', { codes: pending.slice(-MAX_PENDING_PAIRINGS), failures: 0, used: before.used || [] });
      return json({ code: `${code.slice(0, 4)}-${code.slice(4)}`, expiresAt, owner: ownerName });
    }

    if (path === '/ingest/test') {
      const body = await this.usageSummary({ role: 'user' });
      const result = await this.sendToMany((d) => !isOwner(d), { title: 'Claude Usage test', body, tag: 'test' }, { ttl: 600 });
      return json({ delivered: result.delivered, subscribed: result.targets });
    }

    return empty(404);
  }

  // ---- /api: the phone app and the Scriptable widget ------------------------------

  async authenticate(request, { allowWidget = false } = {}) {
    const m = /^Bearer (\S{20,200})$/.exec(request.headers.get('Authorization') || '');
    if (!m) return null;
    const hash = await sha256Hex(m[1]);
    const devices = await this.devices();
    const device = devices.find((d) => d.tokenHash === hash);
    if (device) return { device, devices, scope: 'device' };
    if (allowWidget) {
      const owner = devices.find((d) => d.widgetTokenHash && d.widgetTokenHash === hash);
      if (owner) return { device: owner, devices, scope: 'widget' };
    }
    return null;
  }

  async handleApi(request, path) {
    const method = request.method;

    if (path === '/api/pair' && method === 'POST') return this.pair(request);

    // From the phone's service worker as a push arrives ("received") and once iOS has it on screen
    // ("shown", or "error" if showing it failed). Only a receipt id this phone was sent counts.
    if (path === '/api/receipt' && method === 'POST') {
      const body = await this.readJson(request);
      if (typeof body.d !== 'string' || typeof body.r !== 'string' || !['received', 'shown', 'error'].includes(body.stage)) return empty(400);
      const devices = await this.devices();
      const d = devices.find((x) => x.id === body.d);
      const push = d && (d.pushes || []).find((p) => p.rid === body.r);
      if (!push) return empty(404);
      push[body.stage] = Date.now();
      if (body.stage === 'error') push.why = String(body.info || '').slice(0, 120);
      if (body.stage === 'shown' && Number.isFinite(body.info)) push.onScreen = body.info;
      await this.saveDevices(devices);
      return empty(204);
    }

    if (path === '/api/widget' && method === 'GET') {
      const auth = await this.authenticate(request, { allowWidget: true });
      if (!auth) return json({ error: 'Invalid widget key.' }, 401);
      if (isOwner(auth.device)) {  // an owner's widget shows their own account only
        const view = await this.ownerView(auth.device);
        return json({
          serverTime: Date.now(), usage: view.account, usageReceivedAt: view.accountsReceivedAt,
          accounts: view.account ? [view.account] : [], accountsReceivedAt: view.accountsReceivedAt,
        });
      }
      // The user's widgets: every account (rings per account), and the PC's live token speed.
      const tokens = await this.latestTokens();
      return json({
        serverTime: Date.now(),
        usage: (await this.storage.get('usage')) || null,
        usageReceivedAt: (await this.storage.get('usageReceivedAt')) || 0,
        accounts: await this.accountsForUser((await this.storage.get('accounts')) || {}),
        accountsReceivedAt: (await this.storage.get('accountsReceivedAt')) || 0,
        tokens: tokens && { rate: tokens.rate, at: tokens.at },
      });
    }

    const auth = await this.authenticate(request);
    if (!auth) return json({ error: 'This device is not paired.' }, 401);
    const { device, devices } = auth;
    const owner = isOwner(device);

    if (path === '/api/status' && method === 'GET') {
      // When this phone last opened the app (every 10 minutes at most): the health check shows which
      // pairings are in use and which are copies of the app that were deleted.
      if (Date.now() - (device.seenAt || 0) > 10 * 60_000) {
        device.seenAt = Date.now();
        await this.saveDevices(devices);
      }
      const deviceInfo = { id: device.id, name: device.name, subscribed: !!device.subscription, prefs: device.prefs, hasWidgetKey: !!device.widgetTokenHash,
        // The end of the push address the server holds, so the phone can tell when its own changed.
        endpointTail: device.subscription ? device.subscription.endpoint.slice(-24) : null };
      if (owner) {
        return json({
          role: 'owner', serverTime: Date.now(), vapidPublicKey: this.env.VAPID_PUBLIC_KEY, device: deviceInfo,
          ...(await this.ownerView(device)), tokens: await this.tokensForOwner(device),
          usageSeries: ((await this.storage.get('usageHistory')) || {})[device.ownerEmail] || [],
          reserve: await this.reserveFor(device.ownerEmail),
          usageCheck: (await this.storage.get('usageCheck')) || null,
        });
      }
      const accounts = (await this.storage.get('accounts')) || {};
      return json({
        role: 'user',
        serverTime: Date.now(),
        usage: (await this.storage.get('usage')) || null,
        usageReceivedAt: (await this.storage.get('usageReceivedAt')) || 0,
        // Every account (the user owns the PC, so sees them all), active one first.
        accounts: await this.accountsForUser(accounts),
        tokens: await this.latestTokens(),
        pendingSwitch: await (async () => {
          const r = await this.storage.get('switchRequest');
          return r && Date.now() - r.at < SWITCH_REQUEST_TTL_MS ? r.name : null;
        })(),
        accountsReceivedAt: (await this.storage.get('accountsReceivedAt')) || 0,
        usageCheck: (await this.storage.get('usageCheck')) || null,
        vapidPublicKey: this.env.VAPID_PUBLIC_KEY,
        device: deviceInfo,
      });
    }

    // The phone's live tokens card polls this every 2 s while it is on screen. A friend gets it
    // only while their account is in use.
    if (path === '/api/tokens' && method === 'GET') {
      return json({ serverTime: Date.now(), tokens: owner ? await this.tokensForOwner(device) : await this.latestTokens() });
    }

    // A friend frees what their 5-hour cap keeps for them (until the reset), or takes it back.
    if (path === '/api/reserve' && method === 'POST') {
      if (!owner) return json({ error: 'Only an account owner can do that.' }, 403);
      const body = await this.readJson(request);
      const email = device.ownerEmail;
      const before = await this.reserveFor(email);
      if (!before) return json({ error: 'Nothing is held back for you right now.' }, 409);
      const reserves = (await this.storage.get('reserves')) || {};
      const acc = ((await this.storage.get('accounts')) || {})[email];
      const name = (acc && acc.name) || device.ownerName || 'Your friend';
      const left = duration(before.until - Date.now() / 1000);
      if (body.release === true) {
        if (!before.released) {
          reserves[email] = { until: before.until, cap: before.cap, at: Date.now() };
          await this.storage.put('reserves', reserves);
          await this.sendToMany((d) => !isOwner(d) && d.prefs.reserve_released !== false, {
            title: `${name}: the rest of the 5 hours is yours`,
            body: `${name} won't need their ${before.free}% before it resets in ${left}. Use it until then.`,
            tag: `reserve_${email}`,
          }, { topic: 'reserve_released' });
        }
      } else if (before.released) {
        delete reserves[email];
        await this.storage.put('reserves', reserves);
        await this.sendToMany((d) => !isOwner(d) && d.prefs.reserve_released !== false, {
          title: `${name} wants their reserve back`,
          body: `Back to the ${before.cap}% cap on ${name}.`,
          tag: `reserve_${email}`,
        }, { topic: 'reserve_released' });
      }
      return json({ reserve: await this.reserveFor(email) });
    }

    // An owner's automatic 5-hour cap on their own account: on (it follows the suggested cap,
    // quietly) or off (the cap they had goes back). The user isn't notified either way.
    if (path === '/api/owner/auto-cap' && method === 'PUT') {
      if (!owner) return json({ error: 'Only an account owner can set this.' }, 403);
      const body = await this.readJson(request);
      const email = device.ownerEmail;
      const policies = (await this.storage.get('policies')) || {};
      const ownerAutoCaps = (await this.storage.get('ownerAutoCaps')) || {};
      const p = policies[email] || {};
      let changed = false;
      if (body.on === true && !ownerAutoCaps[email]) {
        const cap = (await this.suggestedCaps()).caps[email];
        if (!cap) return json({ error: 'Not enough is known about your account yet.' }, 409);
        ownerAutoCaps[email] = { before: p.capFiveHour || null };
        policies[email] = { capWeekly: null, allow: true, ...p, capFiveHour: cap, updatedAt: Date.now() };
        changed = true;
      } else if (body.on === false && ownerAutoCaps[email]) {
        policies[email] = { capWeekly: null, allow: true, ...p, capFiveHour: ownerAutoCaps[email].before, updatedAt: Date.now() };
        delete ownerAutoCaps[email];
        changed = true;
      }
      await this.storage.put('ownerAutoCaps', ownerAutoCaps);
      await this.storage.put('policies', policies);
      // One notification to the user when it's switched on or off (not as it moves).
      if (changed) {
        const accounts = (await this.storage.get('accounts')) || {};
        const name = (accounts[email] && accounts[email].name) || device.ownerName || email.split('@')[0];
        const cap = policies[email].capFiveHour;
        await this.sendToMany((d) => !isOwner(d) && d.prefs.access_changes !== false, body.on
          ? { title: `${name} turned on Auto-cap`, body: 'Optimizing their weekly limit for the best token usage.', tag: `auto_${email}` }
          : { title: `${name} turned off Auto-cap`, body: cap ? `Back to their ${cap}% cap.` : 'Back to no cap.', tag: `auto_${email}` },
        { topic: 'auto_cap' });
      }
      return json(await this.ownerView(device));
    }

    // Owner controls: a cap per window (percent used) and whether the user may use the account.
    if (path === '/api/owner/policy' && method === 'PUT') {
      if (!owner) return json({ error: 'Only an account owner can change this.' }, 403);
      const body = await this.readJson(request);
      const cap = capValue;
      const capFiveHour = cap(body.cap_five_hour);
      const capWeekly = cap(body.cap_weekly);
      if (capFiveHour === undefined || capWeekly === undefined || typeof body.allow !== 'boolean') {
        return json({ error: 'Caps must be 5-100 or off; allow must be true or false.' }, 400);
      }
      const policies = (await this.storage.get('policies')) || {};
      const before = policies[device.ownerEmail] || {};
      const policy = { capFiveHour, capWeekly, allow: body.allow, updatedAt: Date.now() };
      policies[device.ownerEmail] = policy;
      await this.storage.put('policies', policies);
      const ownerAutoCaps = (await this.storage.get('ownerAutoCaps')) || {};
      if (ownerAutoCaps[device.ownerEmail] && (before.capFiveHour || null) !== capFiveHour) {
        delete ownerAutoCaps[device.ownerEmail];
        await this.storage.put('ownerAutoCaps', ownerAutoCaps);
      }

      // Tell the user right away on their phone; the laptop applies it on its next upload.
      const accounts = (await this.storage.get('accounts')) || {};
      const name = (accounts[device.ownerEmail] && accounts[device.ownerEmail].name) || device.ownerName || device.ownerEmail.split('@')[0];
      let text = null;
      if (before.allow !== false && !policy.allow) text = 'Asked you to stop using it.';
      else if (before.allow === false && policy.allow) text = 'You can use it again.';
      else if (before.capFiveHour !== capFiveHour || before.capWeekly !== capWeekly) {
        const parts = [capFiveHour ? `5h ${capFiveHour}%` : null, capWeekly ? `weekly ${capWeekly}%` : null].filter(Boolean);
        text = parts.length ? `Capped at ${parts.join(', ')}.` : 'Caps removed.';
      }
      if (text) {
        await this.sendToMany((d) => !isOwner(d) && d.prefs.access_changes !== false,
          { title: `${name} updated access`, body: text, tag: 'owner_policy' }, { topic: 'owner_policy' });
      }
      return json({ policy: { cap_five_hour: capFiveHour, cap_weekly: capWeekly, allow: policy.allow } });
    }

    // The refresh button (anyone's phone): ask the PC for a fresh usage check. The PC still keeps
    // to Anthropic's rate limit, so tapping it a lot just gets the newest numbers.
    if (path === '/api/refresh' && method === 'POST') {
      await this.storage.put('refreshRequest', Date.now());
      return json({ requested: true });
    }

    // Switch accounts from the phone: the PC picks it up within seconds (see /ingest/commands).
    if (path === '/api/switch' && method === 'POST') {
      if (owner) return json({ error: 'Only the account user can switch.' }, 403);
      const body = await this.readJson(request);
      const accounts = (await this.storage.get('accounts')) || {};
      const entry = Object.entries(accounts).find(([, a]) => a.name === body.account);
      if (!entry) return json({ error: 'No such account.' }, 404);
      // (Not refused when it looks in use already: the PC knows better than this possibly
      // seconds-old view, and ignores a switch to the account it's on.)
      await this.storage.put('switchRequest', { email: entry[0], name: entry[1].name, at: Date.now() });
      return json({ requested: entry[1].name });
    }

    // The user's own cap on an account (agreed with its owner, say). The lower cap wins.
    if (path === '/api/my-cap' && method === 'PUT') {
      if (owner) return json({ error: 'Only the account user can set this.' }, 403);
      const body = await this.readJson(request);
      const accounts = (await this.storage.get('accounts')) || {};
      const entry = Object.entries(accounts).find(([, a]) => a.name === body.account);
      if (!entry) return json({ error: 'No such account.' }, 404);
      const capFiveHour = capValue(body.cap_five_hour);
      const capWeekly = capValue(body.cap_weekly);
      if (capFiveHour === undefined || capWeekly === undefined) return json({ error: 'Caps must be 5-100 or off.' }, 400);
      const myCaps = (await this.storage.get('myCaps')) || {};
      const before = myCaps[entry[0]] || {};
      if (capFiveHour || capWeekly) myCaps[entry[0]] = { capFiveHour, capWeekly };
      else delete myCaps[entry[0]];
      await this.storage.put('myCaps', myCaps);
      // A 5-hour cap set by hand ends the automatic one.
      const autoCaps = (await this.storage.get('autoCaps')) || {};
      if (autoCaps[entry[0]] && (before.capFiveHour || null) !== capFiveHour) {
        delete autoCaps[entry[0]];
        await this.storage.put('autoCaps', autoCaps);
      }
      // The owner hears about it, as the user hears about the owner's caps.
      if ((before.capFiveHour || null) !== capFiveHour || (before.capWeekly || null) !== capWeekly) {
        const userName = (await this.storage.get('userName')) || 'Your friend';
        const parts = [capFiveHour ? `5h ${capFiveHour}%` : null, capWeekly ? `weekly ${capWeekly}%` : null].filter(Boolean);
        await this.sendToMany((d) => isOwner(d) && d.ownerEmail === entry[0] && d.prefs.cap_changes !== false, {
          title: parts.length ? `${userName} capped their use` : `${userName} removed their cap`,
          body: parts.length ? `On your account: ${parts.join(', ')}.` : 'Your own caps still apply.',
          tag: 'owner_policy',
        }, { topic: 'owner_policy' });
      }
      return json({ caps: (await this.accountsForUser({ [entry[0]]: entry[1] }))[0].caps });
    }

    // Automatic 5-hour cap on an account: on (it follows the suggested cap from now on, quietly) or
    // off (the cap set before goes back). Nobody is notified either way.
    // The user's own choice: the PC never switches to this account by itself (by hand still works).
    if (path === '/api/no-auto' && method === 'PUT') {
      if (owner) return json({ error: 'Only the account user can set this.' }, 403);
      const body = await this.readJson(request);
      const accounts = (await this.storage.get('accounts')) || {};
      const entry = Object.entries(accounts).find(([, a]) => a.name === body.account);
      if (!entry || typeof body.on !== 'boolean') return json({ error: 'No such account.' }, 404);
      const noAuto = (await this.storage.get('noAuto')) || {};
      if (body.on) noAuto[entry[0]] = true;
      else delete noAuto[entry[0]];
      await this.storage.put('noAuto', noAuto);
      return json({ no_auto: !!noAuto[entry[0]] });
    }

    if (path === '/api/auto-cap' && method === 'PUT') {
      if (owner) return json({ error: 'Only the account user can set this.' }, 403);
      const body = await this.readJson(request);
      const accounts = (await this.storage.get('accounts')) || {};
      const entry = Object.entries(accounts).find(([, a]) => a.name === body.account);
      if (!entry) return json({ error: 'No such account.' }, 404);
      const [email, acc] = entry;
      const myCaps = (await this.storage.get('myCaps')) || {};
      const autoCaps = (await this.storage.get('autoCaps')) || {};
      const mine = myCaps[email] || {};
      let changed = false;
      if (body.on === true && !autoCaps[email]) {
        const cap = (await this.suggestedCaps()).caps[email];
        if (!cap) return json({ error: `Not enough is known about ${acc.name} yet.` }, 409);
        autoCaps[email] = { before: mine.capFiveHour || null };
        myCaps[email] = { capFiveHour: cap, capWeekly: mine.capWeekly || null };
        changed = true;
      } else if (body.on === false && autoCaps[email]) {
        const back = autoCaps[email].before;
        delete autoCaps[email];
        if (back || mine.capWeekly) myCaps[email] = { capFiveHour: back, capWeekly: mine.capWeekly || null };
        else delete myCaps[email];
        changed = true;
      }
      await this.storage.put('autoCaps', autoCaps);
      await this.storage.put('myCaps', myCaps);
      // One notification to the account's owner when it's switched on or off (not as it moves).
      if (changed) {
        const userName = (await this.storage.get('userName')) || 'Your friend';
        const cap = (myCaps[email] || {}).capFiveHour;
        await this.sendToMany((d) => isOwner(d) && d.ownerEmail === email && d.prefs.cap_changes !== false, body.on
          ? { title: `${userName} turned on Auto-cap`, body: 'Optimizing your weekly limit for the best token usage.', tag: `auto_${email}` }
          : { title: `${userName} turned off Auto-cap`, body: cap ? `Back to their ${cap}% cap.` : 'Back to no cap.', tag: `auto_${email}` },
        { topic: 'auto_cap' });
      }
      return json({ caps: (await this.accountsForUser({ [email]: acc }))[0].caps });
    }

    // "Wrapping up a conversation": the user needs the account a moment longer (past a cap the owner
    // set, say) to finish what's in progress. Same rules as the heads-up.
    if (path === '/api/wrap-up' && method === 'POST') {
      if (owner) return json({ error: 'Only the account user can send this.' }, 403);
      const body = await this.readJson(request);
      const accounts = (await this.storage.get('accounts')) || {};
      const entry = Object.entries(accounts).find(([, a]) => a.name === body.account);
      if (!entry) return json({ error: 'No such account.' }, 404);
      const [email, acc] = entry;
      const last = (await this.storage.get('wrapUpAt')) || {};
      if (Date.now() - (last[email] || 0) < HEADS_UP_COOLDOWN_MS) return json({ error: `${acc.name} has just been notified.` }, 429);
      last[email] = Date.now();
      await this.storage.put('wrapUpAt', last);
      const userName = (await this.storage.get('userName')) || 'Your friend';
      return json(await this.sendToMany((d) => isOwner(d) && d.ownerEmail === email && d.prefs.heads_up !== false, {
        title: `${userName} is wrapping up a conversation`,
        body: 'On your account for a moment, just to finish it.',
      }));
    }

    // "I'm about to use your account": the user tells an account's owner ahead of time.
    if (path === '/api/heads-up' && method === 'POST') {
      if (owner) return json({ error: 'Only the account user can send this.' }, 403);
      const body = await this.readJson(request);
      const accounts = (await this.storage.get('accounts')) || {};
      const entry = Object.entries(accounts).find(([, a]) => a.name === body.account);
      if (!entry) return json({ error: 'No such account.' }, 404);
      const [email, acc] = entry;
      const last = (await this.storage.get('headsUpAt')) || {};
      if (Date.now() - (last[email] || 0) < HEADS_UP_COOLDOWN_MS) return json({ error: `${acc.name} has just been notified.` }, 429);
      last[email] = Date.now();
      await this.storage.put('headsUpAt', last);
      const userName = (await this.storage.get('userName')) || 'Your friend';
      const result = await this.sendToMany((d) => isOwner(d) && d.ownerEmail === email && d.prefs.heads_up !== false, {
        title: `${userName} is about to use your account`,
        body: 'Tap for your usage.',  // no tag: each one shows, none replaces another
      });
      return json(result);
    }

    // The user asks a friend for what their 5-hour cap keeps for them, before it resets.
    if (path === '/api/ask-reserve' && method === 'POST') {
      if (owner) return json({ error: 'Only the account user can send this.' }, 403);
      const body = await this.readJson(request);
      const accounts = (await this.storage.get('accounts')) || {};
      const entry = Object.entries(accounts).find(([, a]) => a.name === body.account);
      if (!entry) return json({ error: 'No such account.' }, 404);
      const [email, acc] = entry;
      const r = await this.reserveFor(email);
      if (!r) return json({ error: `No cap is holding anything back on ${acc.name} right now.` }, 409);
      if (r.released) return json({ error: `${acc.name} has already freed it.` }, 409);
      const asked = (await this.storage.get('reserveAsks')) || {};
      const wait = RESERVE_ASK_COOLDOWN_MS - (Date.now() - (asked[email] || 0));
      if (wait > 0) {
        const mins = Math.ceil(wait / 60_000);
        return json({ error: `You can notify ${acc.name} again in ${mins} min${mins === 1 ? '' : 's'}.` }, 429);
      }
      asked[email] = Date.now();
      await this.storage.put('reserveAsks', asked);
      const userName = (await this.storage.get('userName')) || 'Your friend';
      // The reset as the asking phone's clock shows it (e.g. "3:45 PM"), if it sent one.
      const clock = typeof body.clock === 'string' && /^[0-9:. ]{3,8}(AM|PM|am|pm)?$/.test(body.clock.trim()) ? ` (${body.clock.trim()})` : '';
      // Short, so it fits a phone's notification: who asks, and how long until the reset.
      const result = await this.sendToMany((d) => isOwner(d) && d.ownerEmail === email && d.prefs.reserve_asks !== false, {
        title: `${userName} is requesting your remaining usage`,
        body: `${duration(r.until - Date.now() / 1000)} left before reset${clock}`,
        tag: 'reserve_ask', url: '/?reserve',
      }, { topic: 'reserve_asks' });
      return json(result);
    }

    if (path === '/api/subscribe' && method === 'POST') {
      const body = await this.readJson(request);
      const sub = sanitizeSubscription(body.subscription, this.env.TEST_PUSH_ORIGIN);
      if (!sub) return json({ error: 'Invalid push subscription.' }, 400);
      for (const other of devices) {
        if (other !== device && other.subscription && other.subscription.endpoint === sub.endpoint) other.subscription = null;
      }
      device.subscription = sub;
      device.agent = deviceKind(request.headers.get('User-Agent'));
      await this.saveDevices(devices);
      return json({ subscribed: true });
    }

    // The phone's own view of itself, each time the app opens: the health check's last column.
    if (path === '/api/diag' && method === 'POST') {
      const body = await this.readJson(request);
      const str = (v, n) => (typeof v === 'string' ? v.slice(0, n) : null);
      device.diag = {
        at: Date.now(), ua: str(body.ua, 200), standalone: body.standalone === true, permission: str(body.permission, 20),
        sw: str(body.sw, 20), scope: str(body.scope, 120), version: str(body.version, 10),
        // Does the phone's push address match the one the server sends to?
        subMatches: !!(device.subscription && typeof body.sub === 'string' && device.subscription.endpoint.endsWith(body.sub)),
        hasSub: typeof body.sub === 'string',
      };
      await this.saveDevices(devices);
      return empty(204);
    }

    if (path === '/api/subscription' && method === 'DELETE') {
      device.subscription = null;
      await this.saveDevices(devices);
      return json({ subscribed: false });
    }

    if (path === '/api/prefs' && method === 'PUT') {
      const body = await this.readJson(request);
      for (const kind of owner ? OWNER_PREFS : USER_PREFS) {
        if (kind in body) {
          if (typeof body[kind] !== 'boolean') return json({ error: `${kind} must be true or false.` }, 400);
          device.prefs[kind] = body[kind];
        }
      }
      await this.saveDevices(devices);
      return json({ prefs: device.prefs });
    }

    if (path === '/api/test' && method === 'POST') {
      if (!device.subscription) return json({ error: 'Enable notifications first.' }, 409);
      if (Date.now() - (device.lastTestAt || 0) < TEST_COOLDOWN_MS) return json({ error: 'Please wait a few seconds between tests.' }, 429);
      device.lastTestAt = Date.now();
      await this.saveDevices(devices);
      // Sent by the alarm a few seconds from now, so the phone can be locked first.
      const at = nowSeconds() + TEST_DELAY_S;
      const tests = ((await this.storage.get('pendingTests')) || []).filter((x) => x.deviceId !== device.id);
      tests.push({ deviceId: device.id, at });
      await this.storage.put('pendingTests', tests);
      await this.rearm(await this.schedule());
      return json({ scheduledAt: at });
    }

    if (path === '/api/widget-key' && method === 'POST') {
      const key = randomToken();
      device.widgetTokenHash = await sha256Hex(key);
      await this.saveDevices(devices);
      return json({ key }); // shown once; replaces any previous widget key
    }

    if (path === '/api/unpair' && method === 'POST') {
      await this.saveDevices(devices.filter((d) => d.id !== device.id));
      return empty(204);
    }

    return json({ error: 'Not found.' }, 404);
  }

  async pair(request) {
    const now = Date.now();
    if (now - this.pairAttempts.windowStart > 60_000) this.pairAttempts = { windowStart: now, count: 0 };
    if (++this.pairAttempts.count > 10) return json({ error: 'Too many requests, try again shortly.' }, 429);

    const body = await this.readJson(request);
    const code = String(body.code || '').toUpperCase().replace(/[^A-Z0-9]/g, '');
    const codes = await this.pendingPairings();
    if (!codes.length) {
      await this.storage.delete('pairings');
      return json({ error: 'No active pairing code. Run cloud\\pair.ps1 on your PC first.' }, 403);
    }
    const hash = code.length === 8 ? await sha256Hex(code) : '';
    let pairing = null;
    for (const p of codes) if (hash && (await safeEqual(hash, p.codeHash))) pairing = p;
    const stored = (await this.storage.get('pairings')) || {};
    const failures = stored.failures || 0;
    const used = (stored.used || []).filter((u) => u.expiresAt > now);
    if (!pairing && hash && used.some((u) => u.hash === hash)) {
      return json({ error: 'This link was already used. Ask for a new one.' }, 410);  // not a guess
    }
    if (!pairing) {
      // Wrong guesses count against every pending code: after a few, all of them are void.
      if (failures + 1 >= MAX_PAIR_FAILURES) await this.storage.delete('pairings');
      else await this.storage.put('pairings', { codes, failures: failures + 1, used });
      return json({ error: 'Wrong pairing code.' }, 403);
    }
    used.push({ hash, expiresAt: pairing.expiresAt });
    await this.storage.put('pairings', { codes: codes.filter((p) => p !== pairing), failures, used: used.slice(-20) }); // one-time

    const token = randomToken();
    const name = String(body.name || 'iPhone').replace(/[^\w .'-]/g, '').slice(0, 40) || 'iPhone';
    const owner = !!pairing.ownerEmail;
    const devices = await this.devices();
    devices.push({
      id: b64urlEncode(crypto.getRandomValues(new Uint8Array(8))),
      name,
      role: owner ? 'owner' : 'user',
      ownerEmail: pairing.ownerEmail || null,
      ownerName: pairing.ownerName || null,
      tokenHash: await sha256Hex(token),
      widgetTokenHash: null,
      createdAt: new Date().toISOString(),
      subscription: null,
      // Every alert is on until the phone turns it off (missing = on).
      prefs: owner ? { switch_alerts: true, reset_alerts: true } : { five_hour_reset: true, weekly_reset: true, account_switch: true },
      lastTestAt: 0,
    });
    // One phone per friend's account: a new one from their link ends the one before.
    const kept = owner ? devices.filter((d, i) => i === devices.length - 1 || !(isOwner(d) && d.ownerEmail === pairing.ownerEmail)) : devices;
    await this.saveDevices(kept);
    return json({ token, role: owner ? 'owner' : 'user' }, 201);
  }

  async pendingPairings() {
    const stored = await this.storage.get('pairings');
    const now = Date.now();
    return ((stored && stored.codes) || []).filter((p) => now <= p.expiresAt);
  }

  // The user's view of every account, with the caps that apply to it.
  async accountsForUser(accounts) {
    const policies = (await this.storage.get('policies')) || {};
    const myCaps = (await this.storage.get('myCaps')) || {};
    const reserves = (await this.storage.get('reserves')) || {};
    const autoCaps = (await this.storage.get('autoCaps')) || {};
    const noAuto = (await this.storage.get('noAuto')) || {};
    const extras = (await this.storage.get('usageExtras')) || {};
    const resets = (await this.storage.get('resetGrants')) || {};
    const owners = (await this.devices()).filter(isOwner);
    return Object.entries(accounts).map(([email, a]) => {
      const p = policies[email] || {};
      const m = myCaps[email] || {};
      const theirs = owners.filter((d) => d.ownerEmail === email);
      return {
        resets: resets[email] || null,  // limit resets (see ingestAccounts)
        extras: extras[email] || [],  // new on Anthropic's usage data (see ingestAccounts)
        // The owner's phones, and how many can get notifications.
        phones: { paired: theirs.length, notifying: theirs.filter((d) => d.subscription).length },
        ...publicAccount(a),
        caps: {
          mine_five_hour: m.capFiveHour || null, mine_weekly: m.capWeekly || null,
          owner_five_hour: p.capFiveHour || null, owner_weekly: p.capWeekly || null, allow: p.allow !== false,
          // The owner freed their reserve: green from this cap to 100% on the 5-hour bar, until the reset.
          reserve: reserveOpen(reserves[email]) ? { from: reserves[email].cap, until: reserves[email].until } : null,
          auto_five_hour: !!autoCaps[email],
          no_auto: !!noAuto[email],
        },
      };
    });
  }

  // Every account's suggested 5-hour cap right now (see suggestedFiveHourCaps).
  async suggestedCaps() {
    const accounts = (await this.storage.get('accounts')) || {};
    const policies = (await this.storage.get('policies')) || {};
    const myCaps = (await this.storage.get('myCaps')) || {};
    return suggestedFiveHourCaps(accounts, (e) => lower((policies[e] || {}).capWeekly, (myCaps[e] || {}).capWeekly), Date.now() / 1000);
  }

  // What an owner's phone may see: their own account, the user's name, and their rules.
  async ownerView(device) {
    const accounts = (await this.storage.get('accounts')) || {};
    const policies = (await this.storage.get('policies')) || {};
    const acc = accounts[device.ownerEmail] || null;
    const p = policies[device.ownerEmail] || {};
    const mine = ((await this.storage.get('myCaps')) || {})[device.ownerEmail] || {};
    return {
      userName: (await this.storage.get('userName')) || 'Your friend',
      accountsReceivedAt: (await this.storage.get('accountsReceivedAt')) || 0,
      account: acc && publicAccount(acc),
      ownerName: device.ownerName,
      policy: { cap_five_hour: p.capFiveHour || null, cap_weekly: p.capWeekly || null, allow: p.allow !== false },
      extras: ((await this.storage.get('usageExtras')) || {})[device.ownerEmail] || [],
      resets: ((await this.storage.get('resetGrants')) || {})[device.ownerEmail] || null,
      // The user's own cap on this account (the lower of the two applies).
      userCap: { five_hour: mine.capFiveHour || null, weekly: mine.capWeekly || null },
      // Their account's suggested 5-hour cap (worked out with the others, which stay hidden), and
      // whether their own cap follows it.
      suggested: await (async () => {
        const { caps } = await this.suggestedCaps();
        return { five_hour: caps[device.ownerEmail] || null,
          auto: !!((await this.storage.get('ownerAutoCaps')) || {})[device.ownerEmail] };
      })(),
    };
  }
}
