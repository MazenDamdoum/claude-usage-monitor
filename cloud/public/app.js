'use strict';

const TOKEN_KEY = 'claude-usage.device-token';
const WARN = 80;
const CRITICAL = 95;
const ALERTS = ['usage_25', 'usage_50', 'usage_75', 'usage_85', 'usage_100', 'remind_60', 'remind_30', 'remind_15', 'remind_5'];
const PREFS = {
  user: ['five_hour_reset', 'weekly_reset', 'account_switch', 'access_changes', 'reserve_released', ...ALERTS],
  owner: ['heads_up', 'switch_alerts', 'cap_changes', 'reset_alerts', 'reserve_offer', 'reserve_asks', ...ALERTS],
};
const CAPS = [null, ...Array.from({ length: 20 }, (_, i) => (i + 1) * 5)];  // Off, 5%, 10% ... 100%

const $ = (id) => document.getElementById(id);
let token = localStorage.getItem(TOKEN_KEY);
let status = null;
let registration = null;
const FAILED_JOIN_KEY = 'claude-usage.failed-join';
const APP_VERSION = (document.querySelector('meta[name="app-version"]') || {}).content;
// Personal link (/join/ABCD-EFGH): the pairing code is in the address.
const joinCode = (/^\/join\/([A-Za-z0-9]{4}-?[A-Za-z0-9]{4})$/.exec(location.pathname) || [])[1] || null;

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
  const source = currentAccount();
  const w = source && source[key];
  const value = card.querySelector('.value');
  const fill = card.querySelector('.fill');
  const reset = card.querySelector('.reset');
  if (!w) {
    value.textContent = '--';
    value.className = 'value';
    fill.style.width = '0';
    reset.textContent = source ? 'Not on this plan' : 'Waiting for the PC monitor';
    return;
  }
  const pct = Math.round(w.percent);
  value.textContent = `${pct}%`;
  value.className = `value ${severity(pct)}`;
  fill.style.width = `${Math.min(100, Math.max(0, w.percent))}%`;
  setCapMark(card.querySelector('.cap-mark'), capFor(source, key));
  fill.className = `fill ${severity(pct)}`;
  const now = Date.now() / 1000;
  reset.textContent = w.resetsAt > 0 ? `Resets in ${duration(w.resetsAt - now)} · ${clock(w.resetsAt)}` : 'No active window';
}

const isOwnerView = () => status && status.role === 'owner';

// The cap in force on an account's limit: the lower of the owner's and the user's own.
function capFor(acc, key) {
  const five = key === 'five_hour';
  const lowest = (a, b) => (a && b ? Math.min(a, b) : a || b || null);
  if (isOwnerView()) {
    const p = status.policy || {};
    const u = status.userCap || {};
    return lowest(five ? p.cap_five_hour : p.cap_weekly, five ? u.five_hour : u.weekly);
  }
  const c = (acc && acc.caps) || {};
  return lowest(five ? c.mine_five_hour : c.mine_weekly, five ? c.owner_five_hour : c.owner_weekly);
}

function setCapMark(mark, cap) {
  if (!mark) return;
  mark.hidden = !cap;
  if (cap) mark.style.left = `${cap}%`;
}

// The user's phone sees every account on the PC; a friend's phone only theirs.
function accountList() {
  return status && !isOwnerView() && Array.isArray(status.accounts) ? status.accounts : [];
}

// The single account shown on a friend's phone (or with only one account, no claude-swap).
function currentAccount() {
  if (!status) return null;
  return isOwnerView() ? status.account : status.usage;
}

// About how many prompts fit: remaining percent / percent one prompt uses, for whichever
// limit runs out first.
function promptsLeft(acc) {
  const cost = acc && acc.prompt_cost;
  const now = Date.now() / 1000;
  const out = { known: false, limitedBy: null };
  for (const effort of ['low', 'medium', 'high']) {
    let left = null;
    for (const [key, label] of [['five_hour', '5-hour'], ['seven_day', 'weekly']]) {
      const c = cost && cost[key] && cost[key][effort];
      const w = acc && acc[key];
      if (!c || !w) continue;
      const used = w.resetsAt > 0 && w.resetsAt <= now ? 0 : w.percent;
      const n = Math.max(0, Math.floor((100 - used) / c));
      if (left === null || n < left) {
        left = n;
        if (effort === 'medium') out.limitedBy = label;
      }
    }
    if (left !== null) out.known = true;
    out[effort] = left === null ? '--' : left > 999 ? '999+' : String(left);
  }
  return out;
}

// A friend's phone: the prompts-left card for their account.
function renderPrompts() {
  const card = $('prompts-card');
  const acc = currentAccount();
  card.hidden = true;  // the Opus prompts-left estimate is turned off for now (kept for later)
  if (card.hidden || !acc) return;
  const p = promptsLeft(acc);
  for (const effort of ['low', 'medium', 'high']) $(`prompts-${effort}`).textContent = p[effort];
  $('prompts-note').textContent = p.known
    ? `Until the ${p.limitedBy} limit · based on ${status.userName}'s usual Opus prompts`
    : 'Learning from Claude Code use on this account. Shows up after a few prompts.';
}

// ---- the account holder's master page: every account at a glance --------------------

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function feedback(card, text, isError = false) {
  const f = card.querySelector('.acct-feedback');
  f.textContent = text;
  f.className = `acct-feedback small ${isError ? 'error' : 'muted'}`;
  clearTimeout(f.timer);
  f.timer = setTimeout(() => { f.textContent = ''; }, 6000);
}

function buildCard(name) {
  const card = el('section', 'card acct');
  const head = el('div', 'acct-head');
  const switchButton = el('button', 'switch-btn', 'Switch');
  switchButton.type = 'button';
  switchButton.addEventListener('click', () => switchTo(name, card, switchButton));
  // A small phone by the name: green when its owner's phone can be notified, faint when not.
  const phone = el('span', 'acct-phone');
  phone.innerHTML = '<svg viewBox="0 0 24 24" width="13" height="13" aria-hidden="true"><rect x="7" y="2.5" width="10" height="19" rx="2.5" fill="none" stroke="currentColor" stroke-width="2"/><circle cx="12" cy="18" r="1.2" fill="currentColor"/></svg>';
  // Auto: whether the PC may switch to this account by itself. Off = by hand only.
  const autoSw = el('button', 'autosw-btn');
  autoSw.type = 'button';
  autoSw.addEventListener('click', () => setNoAuto(name, card, !autoSw.classList.contains('off')));
  head.append(el('span', 'acct-name', name), phone, el('span', 'acct-state status'), autoSw, switchButton);
  card.append(head);
  for (const [key, label] of [['five_hour', '5h'], ['seven_day', 'Week']]) {
    const row = el('div', 'meter');
    row.dataset.key = key;
    const bar = el('div', 'bar');
    bar.append(el('div', 'fill'), Object.assign(el('div', 'reserve'), { hidden: true }),
      Object.assign(el('div', 'cap-mark'), { hidden: true }));
    row.append(el('span', 'meter-label', label), bar, el('span', 'meter-pct'), el('span', 'meter-reset muted'));
    card.append(row);
  }
  // Two rows: your cap (yours to change), then the owner's (shown greyed out, theirs to change).
  for (const mine of [true, false]) {
    const caps = el('div', `acct-caps small${mine ? '' : ' theirs'}`);
    caps.append(el('span', 'cap-who muted', mine ? 'Your cap' : `${name}'s cap`));
    for (const [key, label] of [['five_hour', '5h'], ['weekly', 'Week']]) {
      const lab = el('label', 'cap');
      const select = document.createElement('select');
      select.dataset.cap = key;
      select.dataset.mine = String(mine);
      select.setAttribute('aria-label', `${mine ? 'Your' : `${name}'s`} ${label} cap on ${name}`);
      for (const cap of CAPS) select.add(new Option(cap ? `${cap}%` : 'Off', cap ? String(cap) : ''));
      if (mine) select.addEventListener('change', () => setMyCapFor(name, card));
      else select.disabled = true;
      lab.append(`${label} `, select);
      caps.append(lab);
    }
    card.append(caps);
  }
  card.append(el('p', 'acct-note muted small'));
  const button = el('button', 'primary acct-btn');
  button.type = 'button';
  button.addEventListener('click', () => sendHeadsUpFor(name, card));
  // Asking for what their 5-hour cap keeps for them (shown while a cap holds some back).
  const ask = el('button', 'secondary acct-ask');
  ask.type = 'button';
  ask.hidden = true;
  ask.addEventListener('click', () => askReserveFor(name, card));
  // "Wrapping up a conversation": on it a moment longer, just to finish.
  const wrap = el('button', 'secondary acct-wrap');
  wrap.type = 'button';
  wrap.addEventListener('click', () => sendWrapUpFor(name, card));
  card.append(button, wrap, ask, el('p', 'acct-feedback small'));
  return card;
}

function updateCard(card, a) {
  const now = Date.now() / 1000;
  card.classList.toggle('active', !!a.active);
  // The light sits with the words, on the right: green in use, red not.
  card.querySelector('.acct-state').replaceChildren(el('span', a.active ? 'light on' : 'light'), a.active ? 'In use' : 'Not in use');
  const autoSw = card.querySelector('.autosw-btn');
  const noAuto = !!(a.caps && a.caps.no_auto);
  autoSw.classList.toggle('off', noAuto);
  autoSw.textContent = noAuto ? 'No auto' : 'Auto';
  autoSw.title = noAuto ? `Never switch to ${a.name} by itself (tap to allow)` : `May switch to ${a.name} by itself (tap to stop)`;
  const sw = card.querySelector('.switch-btn');
  sw.hidden = !!a.active;
  const pending = status.pendingSwitch === a.name;
  if (pending) sw.textContent = 'Switching…';
  else if (sw.dataset.confirm !== '1') sw.textContent = 'Switch';
  sw.disabled = pending;
  for (const row of card.querySelectorAll('.meter')) {
    const w = a[row.dataset.key];
    const pct = w ? Math.round(w.percent) : null;
    const value = row.querySelector('.meter-pct');
    const fill = row.querySelector('.fill');
    value.textContent = pct === null ? '--' : `${pct}%`;
    value.className = `meter-pct ${pct === null ? '' : severity(pct)}`;
    fill.style.width = `${w ? Math.min(100, Math.max(0, w.percent)) : 0}%`;
    fill.className = `fill ${pct === null ? '' : severity(pct)}`;
    row.querySelector('.meter-reset').textContent = w && w.resetsAt > 0 ? duration(w.resetsAt - now) : '';
    // Freed by its owner: green from their cap (or what's used, if more) to the end of the bar.
    setCapMark(row.querySelector('.cap-mark'), row.dataset.key === 'five_hour' && a.caps && a.caps.reserve
      ? a.caps.reserve.from : capFor(a, row.dataset.key));
    const res = row.dataset.key === 'five_hour' && a.caps && a.caps.reserve;
    const green = row.querySelector('.reserve');
    green.hidden = !res;
    if (res) green.style.left = `${Math.min(100, Math.max(res.from, w ? w.percent : 0))}%`;
  }
  const caps = a.caps || {};
  for (const select of card.querySelectorAll('select')) {
    if (document.activeElement === select) continue;
    const five = select.dataset.cap === 'five_hour';
    const v = select.dataset.mine === 'true' ? (five ? caps.mine_five_hour : caps.mine_weekly)
      : (five ? caps.owner_five_hour : caps.owner_weekly);
    // A cap that isn't one of the menu's steps (set some other way) still shows.
    if (v && ![...select.options].some((o) => o.value === String(v))) select.add(new Option(`${v}%`, String(v)));
    select.value = v ? String(v) : '';
  }
  card.querySelector('.acct-note').textContent = caps.allow === false ? `${a.name} asked you to stop using it.`
    : caps.reserve ? `${a.name} freed their reserve: use it until the reset in ${duration(caps.reserve.until - now)}.` : '';
  card.querySelector('.acct-btn').textContent = `Let ${a.name} know I'm about to use it`;
  card.querySelector('.acct-wrap').textContent = `Let ${a.name} know I'm wrapping up a convo`;
  // Whether the owner can be notified at all.
  const ph = a.phones;
  const phone = card.querySelector('.acct-phone');
  phone.hidden = !ph;
  if (ph) {
    phone.classList.toggle('on', ph.notifying > 0);
    phone.title = ph.notifying ? `${a.name} gets notifications` : ph.paired ? `${a.name}'s notifications are off` : `${a.name} hasn't paired a phone`;
    phone.setAttribute('aria-label', phone.title);
  }
  const five = a.five_hour;
  const cap = capFor(a, 'five_hour');
  const ask = card.querySelector('.acct-ask');
  ask.hidden = !cap || caps.reserve || !five || !(five.resetsAt > now) || five.percent >= 100;
  ask.textContent = `Ask ${a.name} for their ${100 - Math.max(Math.round(five ? five.percent : 0), cap || 0)}% reserve`;
}

// ---- Live tokens (the user's phone) ------------------------------------------------------
// The same as the taskbar: tokens per second, rolling up and down (or, if the PC is set to count
// them up, the pile written in this stretch of work, which only climbs), with Claude's blossom
// after it turning faster the more tokens flow, and the speed graphed over the last 5 minutes.
// The PC sends it every 2 s while Claude Code is writing; it is drawn every frame.
const SPIN = ['\u00B7', '\u2722', '\u2733', '\u2736', '\u273B', '\u273D'];
const SPIN_FRAMES = [...SPIN, ...[...SPIN].reverse()];  // Claude Code's order: out and back
const STILL_FRAME = 4;  // the blossom, standing still
const ORANGE = '#d97757';
const live = { data: null, got: 0, shown: 0, count: 0, pile: null, axis: null, phase: 0, turn: 0, last: 0, raf: 0, usageMode: false, usagePct: 0 };

function fmtRate(n) {  // 0-999, then 1.0k and on (as on the taskbar)
  if (n < 999.5) return String(Math.round(n));
  return n < 99950 ? `${(n / 1000).toFixed(1)}k` : `${Math.round(n / 1000)}k`;
}

function fmtTokens(n) {  // 842, 5,843, 123k, 1.2M: every token shows up to 99,999 (rounded down)
  n = Math.floor(n);
  if (n < 100000) return n.toLocaleString('en-US');
  if (n < 1000000) return `${Math.floor(n / 1000)}k`;
  return `${Math.floor(n / 1000000)}.${Math.floor(n / 100000) % 10}M`;
}

// Milliseconds per frame: Claude Code's own pace around 100 tok/s, quicker up to 5,000.
function spinInterval(rate) {
  if (rate < 1) return 0;
  return 230 - 190 * Math.min(1, Math.log10(rate) / Math.log10(5000));
}

// Tokens left in the 5-hour window, written as the taskbar writes it ("842.3k tokens left").
function fmtLeft(n) {
  if (n < 0) return '';
  if (n < 1000) return `${Math.round(n)} tokens left`;
  if (n < 1e6) return `${(n / 1000).toFixed(1)}k tokens left`;
  return `${(n / 1e6).toFixed(2)}M tokens left`;
}

function setTokens(tokens, serverTime) {
  // Its age by the server's clock, so a phone clock that is off doesn't matter.
  live.data = { ...tokens, age0: Math.max(0, ((serverTime || tokens.at) - tokens.at) / 1000) };
  live.got = performance.now();
}

function renderTokens() {
  const card = $('tokens-card');
  const owner = isOwnerView();
  // A friend's phone: the PC's token speed while their account is in use there; otherwise (also
  // while they use it themselves) their own 5-hour usage over the last 5 hours.
  live.usageMode = false;  // a friend sees the token graph too (their own account's share of it)
  const show = !!status.tokens;
  card.hidden = !show;
  if (!show) return;
  setText('tokens-title', live.usageMode ? 'Live usage' : 'Live tokens');
  setText('tokens-caption', !owner ? 'All Claude Code sessions'
    : status.account && status.account.active ? `${status.userName || 'Your friend'} on your account`
      : `${status.userName || 'Your friend'} isn't on your account`);
  if (live.usageMode) {
    const w = status.account.five_hour;
    live.usagePct = !w || w.resetsAt * 1000 <= Date.now() ? 0 : Math.min(100, w.percent);
  } else if (status !== live.seen) {  // a fresh status (not the same one redrawn each second)
    live.seen = status;
    if (!live.data || status.tokens.at >= live.data.at) setTokens(status.tokens, status.serverTime);
  }
  if (!live.raf) live.raf = requestAnimationFrame(drawTokens);
}

async function pollTokens() {
  if (!token || document.hidden || !status || $('tokens-card').hidden) return;
  try {
    const r = await api('GET', '/tokens');
    if (r.tokens && (!live.data || r.tokens.at >= live.data.at)) setTokens(r.tokens, r.serverTime);
  } catch {}
}

function setText(id, text) {
  if ($(id).textContent !== text) $(id).textContent = text;
}

// The light: green "live" while tokens flow, orange "idle" while the account is the one Claude
// Code is signed in to but quiet, red "not in use" when it isn't (on the user's phone: when no
// account is). Returns the word to show.
function setLight(flowing) {
  const inUse = isOwnerView() ? !!(status.account && status.account.active)
    : !accountList().length || accountList().some((a) => a.active);
  const state = !inUse ? 'off' : flowing ? 'live' : 'idle';
  const cls = { live: 'light on', idle: 'light idle', off: 'light' }[state];
  if ($('tokens-light').className !== cls) $('tokens-light').className = cls;
  return { live: 'live', idle: 'idle', off: 'not in use' }[state];
}

function drawTokens(now) {
  live.raf = 0;
  if ($('tokens-card').hidden || document.hidden || (!live.data && !live.usageMode)) return;  // renderTokens starts it again
  const dt = Math.min(0.25, (now - (live.last || now)) / 1000);
  live.last = now;
  if (live.usageMode) {  // a friend's own 5-hour usage
    setText('tokens-rate', String(Math.round(live.usagePct)));
    setText('tokens-unit', '% of 5h');
    // Live while their usage has gone up in the last 10 minutes.
    const series = status.usageSeries || [];
    let rose = 0;
    for (let i = 1; i < series.length; i++) if (series[i][1] > series[i - 1][1]) rose = series[i][0];
    const moving = Date.now() - rose < 10 * 60_000;
    setText('tokens-age', setLight(moving));
    drawBlossom(-1);
    drawChart(0, dt);
    live.raf = requestAnimationFrame(drawTokens);
    return;
  }
  const d = live.data;
  const age = d.age0 + (now - live.got) / 1000;
  const target = age > 30 ? 0 : d.rate;  // the PC went quiet in the middle of a reply
  live.shown += (target - live.shown) * (1 - Math.exp(-dt / 0.15));
  if (target < 0.5 && live.shown < 0.5) live.shown = 0;
  const ms = spinInterval(live.shown);
  if (ms) live.phase = (live.phase + (dt * 1000) / ms) % SPIN_FRAMES.length;
  // The pile: a new stretch starts from 0; between updates it carries on at the current speed
  // (up to 3 s ahead); it climbs toward that and never back down.
  if (d.pile !== live.pile) {
    if (live.pile !== null || d.working) live.count = 0;
    live.pile = d.pile;
  }
  const pile = d.working && age <= 30 ? (d.count || 0) + target * Math.min(age, 3) : 0;
  if (pile > live.count) live.count += (pile - live.count) * (1 - Math.exp(-dt / 0.35));
  const countUp = d.mode === 'pile';
  setText('tokens-rate', countUp ? fmtTokens(live.count) : fmtRate(live.shown));
  setText('tokens-unit', countUp ? 'tokens' : 'tok/s');
  setText('tokens-left', fmtLeft(d.left == null ? -1 : Math.max(0, d.left - target * Math.min(age, 3))));
  // Top right: green and "live" while tokens flow, red and "idle" when they don't (or how long
  // since the PC was last heard from); with the pile as the big number, the speed instead of "live".
  const working = age < 6 && target >= 0.5;
  const word = setLight(working);
  const fresh = word !== 'idle' || age < 90 ? word : `idle · ${duration(Math.round(age))}`;
  setText('tokens-age', countUp && working ? `${fmtRate(target)} tok/s` : fresh);
  drawBlossom(ms ? Math.floor(live.phase) : -1);  // Claude Code's frames, as on the taskbar
  drawChart(age, dt);
  live.raf = requestAnimationFrame(drawTokens);
}

// A canvas sized for the screen's pixels; returns its context, drawing in CSS pixels.
function canvasFor(id) {
  const c = $(id);
  const dpr = window.devicePixelRatio || 1;
  const cw = c.clientWidth;
  const ch = c.clientHeight;
  if (!cw || !ch) return null;
  if (c.width !== Math.round(cw * dpr) || c.height !== Math.round(ch * dpr)) {
    c.width = Math.round(cw * dpr);
    c.height = Math.round(ch * dpr);
  }
  const g = c.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, cw, ch);
  return { g, cw, ch };
}

// The y-axis for tokens per second: round steps (50, 100, 250, 500, 1k...), at most four lines.
// It holds still: it grows the moment a peak needs more room, and shrinks only once the graph has
// fallen well below it, so the numbers don't keep moving.
function tokenAxis(peak) {
  const steps = [50, 100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000];
  const want = Math.max(100, peak * 1.08);
  const step = steps.find((s) => Math.ceil(want / s) <= 4) || steps[steps.length - 1];
  const max = Math.ceil(want / step) * step;
  if (!live.axis || max > live.axis.max || peak < live.axis.max * 0.45) live.axis = { max, step, fmt: fmtRate };
  return live.axis;
}

function drawChart(age, dt) {
  const cv = canvasFor('tokens-chart');
  if (!cv) return;
  const { g, cw, ch } = cv;
  const gutter = 28;  // the y-axis numbers, in their own column on the right
  const padT = 6;
  const padB = 4;
  const w = cw - gutter - 4;
  const h = ch - padT - padB;
  let pts;
  let tip;
  let axis;
  if (live.usageMode) {
    // A friend's 5-hour usage, over the last 5 hours.
    const span = 5 * 3600 * 1000;
    const nowMs = Date.now();
    axis = { max: 100, step: 25, fmt: (v) => `${v}%` };
    tip = live.usagePct;
    pts = (status.usageSeries || []).filter(([t]) => nowMs - t <= span && t <= nowMs)
      .map(([t, v]) => [w * (1 - (nowMs - t) / span), v]);
    setText('tokens-span', '5 h ago');
  } else {
    const d = live.data;
    const fine = Array.isArray(d.series) && d.series.length > 1;
    const values = fine ? d.series : d.history || [];
    const step = fine ? 1 : 60;  // seconds between values
    const span = Math.max(1, values.length * step);
    setText('tokens-span', `${Math.round(span / 60)} min ago`);
    axis = tokenAxis(Math.max(live.shown, ...values));
    tip = live.shown;
    // Each value sits at its own second; the line then runs on to "now" at the live number.
    pts = values.map((v, i) => [w * (1 - ((values.length - 1 - i) * step + age) / span), v]).filter(([x]) => x < w);
  }
  const y = (v) => padT + h - (Math.min(v, axis.max) / axis.max) * h;
  pts = pts.map(([x, v]) => [x, y(v)]);
  pts.push([w, y(tip)]);

  // The grid: a faint line per step across the graph, its number in the column on the right.
  g.font = '500 9px -apple-system, system-ui, sans-serif';
  g.textAlign = 'right';
  g.textBaseline = 'middle';
  for (let v = axis.step; v <= axis.max + 0.001; v += axis.step) {
    const yy = Math.round(y(v)) + 0.5;
    g.strokeStyle = 'rgba(255,255,255,.09)';
    g.lineWidth = 1;
    g.setLineDash([3, 4]);
    g.beginPath();
    g.moveTo(0, yy);
    g.lineTo(w, yy);
    g.stroke();
    g.setLineDash([]);
    g.fillStyle = 'rgba(255,255,255,.42)';
    g.fillText(axis.fmt(v), cw - 1, Math.max(5, yy));
  }
  g.textAlign = 'left';

  const path = () => {
    g.beginPath();
    pts.forEach(([x, yy], i) => (i ? g.lineTo(x, yy) : g.moveTo(x, yy)));
  };
  path();
  g.lineTo(w, ch);
  g.lineTo(pts[0][0], ch);
  g.closePath();
  const fill = g.createLinearGradient(0, padT, 0, ch);
  fill.addColorStop(0, 'rgba(217,119,87,.42)');
  fill.addColorStop(1, 'rgba(217,119,87,0)');
  g.fillStyle = fill;
  g.fill();
  path();
  g.strokeStyle = ORANGE;
  g.lineWidth = 2;
  g.lineJoin = 'round';
  g.stroke();
  g.beginPath();
  g.arc(w, y(tip), 2.6, 0, Math.PI * 2);
  g.fillStyle = ORANGE;
  g.fill();
}

// Claude's blossom, left of the number: the taskbar's own frames (blossom/0-5.png, made from its
// font by make-blossom.ps1, each with its ink at the middle), so it looks the same on the phone and
// never shakes. It is centred on the digits, measured where they really are: #tokens-base marks the
// number's baseline, and the digits' height comes from the number's own font size.
const BLOSSOM = SPIN.map((_, i) => Object.assign(new Image(), { src: `/blossom/${i}.png?v=1` }));
let digitHeight = 0;

function drawBlossom(frame) {
  const cv = canvasFor('tokens-blossom');
  if (!cv) return;
  const { g, cw, ch } = cv;
  if (!digitHeight) {
    const cs = getComputedStyle($('tokens-rate'));  // (its `font` shorthand can come back empty)
    g.font = `${cs.fontWeight} ${cs.fontSize} system-ui, -apple-system, "Segoe UI", sans-serif`;
    digitHeight = g.measureText('0').actualBoundingBoxAscent || parseFloat(cs.fontSize) * 0.7;
  }
  const i = frame >= 0 ? frame : STILL_FRAME;
  const img = BLOSSOM[i < SPIN.length ? i : SPIN_FRAMES.length - 1 - i];
  if (!img.complete || !img.naturalWidth) return;
  const size = Math.min(cw, digitHeight * 0.95);  // the fullest frame a touch shorter than the digits
  const baseline = $('tokens-base').getBoundingClientRect().bottom;
  const y = baseline - digitHeight / 2 - $('tokens-blossom').getBoundingClientRect().top;
  g.globalAlpha = frame >= 0 ? 1 : 0.5;
  g.drawImage(img, cw / 2 - size / 2, y - size / 2, size, size);  // each frame's ink is centred: it never shakes
  g.globalAlpha = 1;
}

// ---- Suggested caps -------------------------------------------------------------------------
// Each account's 5-hour cap, worked out on its own: the most of each 5-hour session that still uses
// up exactly what's left of that account's week (or of your weekly cap on it) at its weekly reset,
// counting every 5-hour window until then, around the clock - the session running now (to its own
// reset, with what it has used so far) and the ones after it. A full 5-hour session costs a fixed
// share of the account's week, which the PC learns (prompt_cost: % of each limit per prompt). The
// cloud works out the same figure, for Auto and for a friend's box.
const PACE_LINE = 'Set your calculated 5-hour caps to maximize your weekly limit.';

function paceFor(a) {
  const now = Date.now() / 1000;
  const week = a.seven_day;
  const five = a.five_hour;
  const pc = a.prompt_cost;
  if (!week || !(week.resetsAt > now)) return { note: 'No weekly limit running' };
  const share = pc && pc.five_hour && pc.seven_day && pc.five_hour.high > 0 && pc.seven_day.high > 0
    ? pc.seven_day.high / pc.five_hour.high : null;  // % of the week per 1% of a 5-hour session
  if (!share) return { note: 'Still learning this account (use it a little)' };
  const caps = a.caps || {};
  const weekCap = Math.min(caps.mine_weekly || 100, caps.owner_weekly || 100);
  const left = Math.max(0, weekCap - week.percent);
  const inSession = !!five && five.resetsAt > now;
  const from = inSession ? Math.min(five.resetsAt, week.resetsAt) : now;
  const sessions = (inSession ? 1 : 0) + Math.max(0, week.resetsAt - from) / (5 * 3600);
  const cap = left <= 0 ? 0 : Math.min(100, (left / share + (inSession ? five.percent : 0)) / Math.max(1, sessions));
  return { cap, left, weekCap, hours: (week.resetsAt - now) / 3600 };
}

function paceAll(list) {
  return { pace: new Map(list.map((a) => [a, paceFor(a)])) };
}

// The Week column: where the week's usage should be right now to reach its cap (100%, or a weekly
// cap set on it) exactly at its reset - the share of the 7 days gone by.
function weekPaceOf(week, weekCap, now) {
  if (!week || !(week.resetsAt > now)) return null;
  const gone = Math.min(1, Math.max(0, 1 - (week.resetsAt - now) / (7 * 86400)));
  return Math.round(weekCap * gone);
}

// "5h" and "Week" over the two columns.
function paceHead() {
  const head = el('div', 'pace-row pace-head');
  head.append(el('span'), el('span', 'pace-col', '5h'), el('span', 'pace-col', 'Week'), el('span'));
  return head;
}

const suggestedOf = (p) => (p.cap <= 0 ? 0 : Math.max(5, Math.floor(p.cap / 5) * 5));

// A friend's own box: their account only, with the suggestion the cloud worked out for it.
function renderOwnerPace() {
  const card = $('pace-card');
  const acc = status.account;
  const sug = status.suggested || {};
  card.hidden = !acc;
  if (card.hidden) return;
  const week = acc.seven_day;
  const now = Date.now() / 1000;
  const row = el('div', 'pace-row');
  const who = el('div');
  who.append(el('div', 'pace-name', acc.name));
  const big = el('span', 'pace-pct');
  const weekCap = Math.min((status.policy || {}).cap_weekly || 100, (status.userCap || {}).weekly || 100);
  const left = week ? Math.max(0, weekCap - week.percent) : 0;
  const top = el('div', 'pace-top');
  top.append(el('span', 'muted small', PACE_LINE));
  if (!sug.five_hour || !week || !(week.resetsAt > now)) {
    big.textContent = '--';
    who.append(el('div', 'pace-detail', !week || !(week.resetsAt > now) ? 'No weekly limit running' : 'Still learning this account (use it a little)'));
    row.append(who, big, el('span', 'pace-pct pace-week', '--'), el('span'));
    $('pace-list').replaceChildren(top, paceHead(), row);
    return;
  }
  const days = duration(week.resetsAt - now);
  const usedUp = left <= 0;
  big.textContent = usedUp ? 'Stop' : `${sug.five_hour}%`;
  who.append(el('div', 'pace-detail', usedUp ? `Week used up \u00B7 resets in ${days}` : `${Math.round(left)}% of week left \u00B7 ${days}`));
  const btn = el('button', sug.auto ? 'pace-auto on' : 'pace-auto', sug.auto ? 'Auto on' : 'Auto');
  btn.type = 'button';
  btn.addEventListener('click', () => (sug.auto ? setOwnerAutoCap(false) : askOwnerAutoCap()));
  const pw = weekPaceOf(week, weekCap, now);
  row.append(who, big, el('span', 'pace-pct pace-week', pw === null ? '--' : `${pw}%`), btn);
  $('pace-list').replaceChildren(top, paceHead(), row);
}

function askOwnerAutoCap() {
  const now = (status.policy || {}).cap_five_hour;
  autoFor = 'owner';
  $('auto-title').textContent = 'Auto-cap your account?';
  $('auto-text').textContent = `Your 5-hour cap follows the suggestion (${status.suggested.five_hour}% now) as usage changes. `
    + `${status.userName || 'Your friend'} isn't notified of these changes. Turn it off to go back to ${now ? `your ${now}% cap` : 'no cap'}.`;
  $('auto-confirm').hidden = false;
  $('auto-confirm').scrollIntoView({ behavior: 'smooth', block: 'center' });
}

async function setOwnerAutoCap(on) {
  $('auto-confirm').hidden = true;
  try {
    status = { ...status, ...(await api('PUT', '/owner/auto-cap', { on })) };
    const cap = (status.policy || {}).cap_five_hour;
    message(on ? 'Auto-cap on for your account.' : `Auto-cap off: back to ${cap ? `your ${cap}% cap` : 'no cap'}.`);
  } catch (err) {
    message(err.message, true);
  }
  renderStatus();
}

// Limit resets: shown only while Anthropic's usage data for an account has something new on it
// (a field the app doesn't know filling in - limit resets, when they come), one short line each:
// the account, when it ends, and the field's name.
function renderExtras() {
  const list = isOwnerView() ? (status.account ? [{ ...status.account, extras: status.extras || [], resets: status.resets }] : []) : accountList();
  const now = Date.now() / 1000;
  const rows = [];
  for (const a of list) {
    // One line per account: how many resets are left on its grant and when it ends.
    const r = a.resets;
    if (r) {
      const live = r.grants.filter((g) => !g.endsAt || g.endsAt > now);
      const left = live.reduce((n, g) => n + g.left, 0);
      const ends = live.map((g) => g.endsAt).filter(Boolean).sort()[0];
      const text = live.length
        ? `${left ? `${left} left` : 'Used'}${ends ? ` · ends ${clock(ends)}` : ''}`
        : r.eligible ? 'None right now' : 'Not eligible';
      const row = el('div', 'extra-row');
      row.append(el('span', 'pace-name', a.name), el('span', left ? 'extra-when' : 'extra-when muted', text));
      rows.push(row);
    }
    for (const x of a.extras || []) {
      const row = el('div', 'extra-row');
      row.append(el('span', 'pace-name', a.name),
        el('span', 'extra-when', x.endsAt ? (x.endsAt > now ? `ends in ${duration(x.endsAt - now)} \u00B7 ${clock(x.endsAt)}` : 'ended') : 'no end date'),
        el('span', 'extra-key muted small', x.key));
      rows.push(row);
    }
  }
  $('extras-card').hidden = !rows.length;
  $('extras-list').replaceChildren(...rows);
}

function renderPace() {
  renderExtras();
  if (isOwnerView()) return renderOwnerPace();
  const card = $('pace-card');
  const list = accountList();
  card.hidden = !list.length;
  if (card.hidden) return;
  const { pace } = paceAll(list);
  // Top: the line, and Auto for every account at once.
  const ready = list.filter((a) => pace.get(a).cap !== undefined);
  const allOn = ready.length > 0 && ready.every((a) => a.caps && a.caps.auto_five_hour);
  const top = el('div', 'pace-top');
  top.append(el('span', 'muted small', PACE_LINE));
  if (ready.length > 1) {
    const all = el('button', allOn ? 'pace-auto on' : 'pace-auto', allOn ? 'Auto all on' : 'Auto all');
    all.type = 'button';
    all.addEventListener('click', () => (allOn
      ? setAutoCap(ready.map((a) => a.name), false)
      : askAutoCap(ready.filter((a) => !(a.caps && a.caps.auto_five_hour)), pace)));
    top.append(all);
  }
  const rows = list.map((a) => {
    const p = pace.get(a);
    const row = el('div', 'pace-row');
    // Left: the account and how much of its week is left. Right: the cap to use now.
    const who = el('div');
    who.append(el('div', 'pace-name', a.name));
    const big = el('span', 'pace-pct');
    const wk = el('span', 'pace-pct pace-week');
    if (p.cap === undefined) {
      big.textContent = '--';
      wk.textContent = '--';
      who.append(el('div', 'pace-detail', p.note));
      row.append(who, big, wk, el('span'));
      return row;
    }
    const pw = weekPaceOf(a.seven_day, p.weekCap, Date.now() / 1000);
    wk.textContent = pw === null ? '--' : `${pw}%`;
    const suggested = suggestedOf(p);
    big.textContent = suggested ? `${suggested}%` : 'Stop';
    const days = duration(p.hours * 3600);
    who.append(el('div', 'pace-detail', p.left <= 0
      ? `Week used up \u00B7 resets in ${days}`
      : `${Math.round(p.left)}% of week left \u00B7 ${days}`));
    const auto = !!(a.caps && a.caps.auto_five_hour);
    const btn = el('button', auto ? 'pace-auto on' : 'pace-auto', auto ? 'Auto on' : 'Auto');
    btn.type = 'button';
    btn.addEventListener('click', () => (auto ? setAutoCap([a.name], false) : askAutoCap([a], pace)));
    row.append(who, big, wk, btn);
    return row;
  });
  $('pace-list').replaceChildren(top, paceHead(), ...rows);
}

// Auto-cap: first the red banner, then the cloud keeps each 5-hour cap at the suggested one.
let autoFor = null;
function askAutoCap(accs, pace) {
  if (!accs.length) return;
  autoFor = accs.map((a) => a.name);
  const names = autoFor.length > 1 ? `${autoFor.slice(0, -1).join(', ')} and ${autoFor[autoFor.length - 1]}` : autoFor[0];
  const now = accs.map((a) => `${a.name} ${suggestedOf(pace.get(a))}%`).join(', ');
  $('auto-title').textContent = accs.length > 1 ? 'Auto-cap every account?' : `Auto-cap ${names}?`;
  $('auto-text').textContent = accs.length > 1
    ? `Your 5-hour caps on ${names} follow the suggestions (${now} now) as usage changes, without notifications. `
      + 'Turn Auto off to go back to the caps you had.'
    : `Your 5-hour cap on ${names} follows the suggestion (${suggestedOf(pace.get(accs[0]))}% now) as usage changes, without notifications. `
      + `Turn it off to go back to ${accs[0].caps && accs[0].caps.mine_five_hour ? `your ${accs[0].caps.mine_five_hour}% cap` : 'no cap'}.`;
  $('auto-confirm').hidden = false;
  $('auto-confirm').scrollIntoView({ behavior: 'smooth', block: 'center' });
}

async function setAutoCap(names, on) {
  $('auto-confirm').hidden = true;
  const done = [];
  try {
    for (const name of names) {
      const { caps } = await api('PUT', '/auto-cap', { account: name, on });
      const acc = accountList().find((x) => x.name === name);
      if (acc) acc.caps = caps;
      done.push([name, caps]);
    }
    if (names.length > 1) message(on ? 'Auto-cap on for every account.' : 'Auto-cap off: every account is back to the cap it had.');
    else if (done.length) {
      const [name, caps] = done[0];
      message(on ? `Auto-cap on for ${name}.` : `Auto-cap off: ${name} is back to ${caps.mine_five_hour ? `your ${caps.mine_five_hour}% cap` : 'no cap'}.`);
    }
  } catch (err) {
    message(err.message, true);
  }
  renderPace();
  renderBoard();
}

function renderBoard() {
  const board = $('board');
  const list = [...accountList()].sort((a, b) => Number(!!b.active) - Number(!!a.active));  // in use on top
  const on = !isOwnerView() && list.length > 0;
  board.hidden = !on;
  document.body.classList.toggle('board', on);
  if (!on) return;
  const names = list.map((a) => a.name).join('|');
  if (board.dataset.names !== names) {
    const old = new Map([...board.children].map((c) => [c.querySelector('.acct-name').textContent, c]));
    board.replaceChildren(...list.map((a) => old.get(a.name) || buildCard(a.name)));
    board.dataset.names = names;
  }
  list.forEach((a, i) => updateCard(board.children[i], a));
}

async function setMyCapFor(name, card) {
  const acc = accountList().find((a) => a.name === name);
  const cap = (key) => {
    const v = card.querySelector(`select[data-cap="${key}"]`).value;
    return v ? Number(v) : null;
  };
  try {
    const { caps } = await api('PUT', '/my-cap', { account: name, cap_five_hour: cap('five_hour'), cap_weekly: cap('weekly') });
    if (acc) acc.caps = caps;
    feedback(card, `Cap saved. Your PC applies it within a minute, and ${name} is notified.`);
  } catch (err) {
    feedback(card, err.message, true);
  }
  renderStatus();
}

// Switch accounts from the phone: two taps (Switch, then Confirm), then the PC does it within
// seconds, as if the column had been clicked on the taskbar.
async function switchTo(name, card, button) {
  if (button.dataset.confirm !== '1') {
    button.dataset.confirm = '1';
    button.textContent = 'Confirm';
    clearTimeout(button.timer);
    button.timer = setTimeout(() => { button.dataset.confirm = ''; button.textContent = 'Switch'; }, 4000);
    return;
  }
  clearTimeout(button.timer);
  button.dataset.confirm = '';
  button.textContent = 'Switching…';
  button.disabled = true;
  try {
    await api('POST', '/switch', { account: name });
    feedback(card, `Your PC switches to ${name} in a few seconds (it needs to be awake).`);
    status.pendingSwitch = name;
    for (const wait of [6000, 12000, 20000, 35000]) setTimeout(() => token && refresh(), wait);
  } catch (err) {
    button.textContent = 'Switch';
    button.disabled = false;
    feedback(card, err.message, true);
  }
}

// The header's countdown to the PC's next usage check with Anthropic (every 2-5 minutes, as often
// as Anthropic's rate limit allows): the ring empties, it checks, the new reading arrives a few
// seconds later with the next time, and it starts again. "Offline" when the PC hasn't sent anything
// for 10 minutes.
function renderCheckTimer() {
  const c = status && status.usageCheck;
  const received = status && status.accountsReceivedAt;
  const now = Date.now() / 1000;
  const offline = !received || Date.now() - received > 10 * 60_000;
  let text = '--';
  let left = 0;
  const checking = !offline && !!c && c.next <= now;
  if (offline) text = 'Offline';
  else if (checking) text = 'Now';
  else if (c) {
    const s = Math.ceil(c.next - now);
    text = `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
    left = Math.min(1, (c.next - now) / Math.max(1, c.next - c.at));
  }
  const btn = $('refresh');
  $('check-time').textContent = text;
  btn.classList.toggle('checking', checking);
  btn.classList.toggle('offline', offline);
  if (!checking) $('check-ring').style.strokeDashoffset = String(100 * (1 - left));
}

// Tapping the countdown: fresh numbers from the cloud now, and a fresh check from the PC (still
// within Anthropic's rate limit), which the phone picks up over the next few seconds.
async function refreshNow() {
  try {
    await api('POST', '/refresh');
  } catch {
    // offline: the status fetch below says so
  }
  await refresh();
  for (const wait of [4000, 9000, 15000]) setTimeout(() => token && refresh(), wait);
}

async function setNoAuto(name, card, on) {
  try {
    const { no_auto } = await api('PUT', '/no-auto', { account: name, on });
    const acc = accountList().find((x) => x.name === name);
    if (acc && acc.caps) acc.caps.no_auto = no_auto;
    feedback(card, no_auto ? `Won't auto-switch to ${name}. Switch by hand any time.` : `May auto-switch to ${name} again.`);
  } catch (err) {
    feedback(card, err.message, true);
  }
  renderBoard();
}

async function sendWrapUpFor(name, card) {
  try {
    const { delivered } = await api('POST', '/wrap-up', { account: name });
    feedback(card, delivered ? `${name} has been notified.` : `${name} hasn't turned on notifications yet.`);
  } catch (err) {
    feedback(card, err.message, true);
  }
}

async function sendHeadsUpFor(name, card) {
  try {
    const { delivered } = await api('POST', '/heads-up', { account: name });
    feedback(card, delivered ? `${name} has been notified.` : `${name} hasn't turned on notifications yet.`);
  } catch (err) {
    feedback(card, err.message, true);
  }
}

// "Can Mazen use your 40% before your 5-hour session resets in 2h 14m (3:45 PM)?"
async function askReserveFor(name, card) {
  const acc = accountList().find((a) => a.name === name);
  const at = acc && acc.five_hour ? new Date(acc.five_hour.resetsAt * 1000).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' }) : '';
  try {
    const { delivered } = await api('POST', '/ask-reserve', { account: name, clock: at });
    feedback(card, delivered ? `${name} has been asked.` : `${name} hasn't turned on notifications yet.`);
  } catch (err) {
    feedback(card, err.message, true);
  }
}

// A friend's phone: their own account only, who is using it, and their rules.
// The friend's reserve: what their 5-hour cap keeps for them. If they won't need it before the
// reset, one tap frees it for the user (and tells them); another takes it back.
function renderReserve() {
  const r = status.reserve;
  $('reserve-box').hidden = !r;
  if (!r) return;
  const left = duration(r.until - Date.now() / 1000);
  const name = status.userName || 'They';
  $('reserve-text').textContent = r.released
    ? `${name} can use the rest of your 5 hours until it resets in ${left}.`
    : `Your ${r.cap}% cap keeps ${r.free}% of your 5 hours for you. Won't need it before the reset in ${left}?`;
  $('reserve-btn').textContent = r.released ? 'Take it back' : `Let ${name} use it`;
  $('reserve-btn').className = r.released ? 'secondary' : '';
}

async function toggleReserve() {
  const r = status && status.reserve;
  if (!r) return;
  $('reserve-btn').disabled = true;
  try {
    const { reserve } = await api('POST', '/reserve', { release: !r.released });
    status.reserve = reserve;
    message(reserve && reserve.released ? `${status.userName} has been notified.` : 'Your reserve is yours again.');
  } catch (err) {
    message(err.message, true);
  }
  $('reserve-btn').disabled = false;
  renderReserve();
}

function renderOwner() {
  renderReserve();
  const name = status.userName;
  const acc = status.account;
  $('owner-user').textContent = name;
  const inUse = $('owner-in-use');
  inUse.replaceChildren();
  if (acc) {
    const light = document.createElement('span');
    light.className = acc.active ? 'light on' : 'light';
    inUse.append(light, acc.active ? 'In use now' : 'Not in use');
  } else {
    inUse.textContent = 'Not seen yet';
  }
  inUse.className = 'status';
  $('owner-allow-label').textContent = `Allow ${name} to use it`;
  $('owner-allow').checked = status.policy.allow;
  for (const [id, key] of [['cap-five_hour', 'cap_five_hour'], ['cap-weekly', 'cap_weekly']]) {
    const select = $(id);
    if (!select.options.length) {
      for (const cap of CAPS) select.add(new Option(cap ? `${cap}%` : 'Off', cap ? String(cap) : ''));
    }
    if (document.activeElement !== select) select.value = status.policy[key] ? String(status.policy[key]) : '';
  }
  const uc = status.userCap || {};
  const theirs = [uc.five_hour ? `5h ${uc.five_hour}%` : null, uc.weekly ? `weekly ${uc.weekly}%` : null].filter(Boolean);
  $('owner-user-cap').hidden = !theirs.length;
  $('owner-user-cap').textContent = `${name} also capped their own use: ${theirs.join(', ')}. The lower cap applies.`;
}

function renderStatus() {
  if (!status) return;
  document.body.classList.toggle('owner', isOwnerView());
  if (isOwnerView()) {
    // A friend's page: their 5-hour and weekly boxes first, then Suggested caps (and limit resets).
    const weekly = $('limit-seven_day');
    if (weekly.nextElementSibling !== $('pace-card')) weekly.after($('pace-card'), $('extras-card'));
    $('title').textContent = status.account ? `${status.account.name}'s Claude` : 'Claude Usage';
    renderOwner();
  } else if ($('pace-card').nextElementSibling !== $('extras-card')) {
    $('pace-card').after($('extras-card'));  // Limit resets right after Suggested caps
  }
  renderCheckTimer();
  renderTokens();
  renderWidgetPreviews();
  renderPace();
  renderBoard();
  renderLimit('five_hour');
  renderLimit('seven_day');
  renderPrompts();

  const received = isOwnerView() ? status.accountsReceivedAt : status.usageReceivedAt;
  const age = received ? Math.round((Date.now() - received) / 1000) : null;
  $('monitor-status').textContent =
    age === null ? 'Waiting for your PC' : age < 300 ? `Live · updated ${duration(age) === '<1m' ? 'just now' : duration(age) + ' ago'}` : `PC offline · last update ${duration(age)} ago`;

  const permission = pushSupported ? Notification.permission : 'unsupported';
  let text;
  if (!pushSupported) text = isIOS && !isStandalone ? 'Add to Home Screen first' : 'Not supported here';
  else if (permission === 'denied') text = 'Blocked in Settings';
  else if (status.device.subscribed && permission === 'granted') text = 'On';
  else text = 'Off';
  $('notify-status').textContent = text;
  // Not on: say so at the top, with the button (or where to allow them, if blocked).
  const banner = $('notify-banner');
  banner.hidden = text === 'On' || !pushSupported;
  if (!banner.hidden) {
    const who = isOwnerView() ? `when ${status.userName || 'your friend'} uses your account or asks for your reserve` : 'about usage, resets and your friends';
    $('notify-banner-text').textContent = text === 'Blocked in Settings'
      ? 'Allow them in Settings > Notifications > Claude Usage.' : `Turn them on to hear ${who}.`;
    $('notify-banner-btn').hidden = text === 'Blocked in Settings';
  }

  const on = text === 'On';
  // Always there: the first tap turns notifications on, later taps confirm they are.
  $('enable').textContent = on ? 'Notifications are on' : 'Enable Notifications';
  $('test-push').hidden = !on;
  for (const kind of PREFS[isOwnerView() ? 'owner' : 'user']) {
    const box = $(`pref-${kind}`);
    box.checked = status.device.prefs[kind] !== false;
    box.disabled = !on;
  }
}

// Once each time the app opens: how this phone sees itself (the PC owner's health check shows it,
// to tell a phone setting from a problem on our side).
let diagSent = false;
async function sendDiag() {
  if (diagSent) return;
  diagSent = true;
  try {
    const sub = pushSupported && registration ? await registration.pushManager.getSubscription() : null;
    const sw = registration && (registration.active || registration.waiting || registration.installing);
    await api('POST', '/diag', {
      ua: navigator.userAgent, standalone: isStandalone,
      permission: pushSupported ? Notification.permission : 'unsupported',
      sw: sw ? sw.state : 'none', scope: registration ? registration.scope : null,
      sub: sub ? sub.endpoint.slice(-24) : null,
      version: (document.querySelector('meta[name="app-version"]') || {}).content || null,
    });
  } catch {
    diagSent = false;
  }
}

async function refresh() {
  try {
    status = await api('GET', '/status');
    renderStatus();
    await healSubscription();
    sendDiag();
  } catch (err) {
    message(err.message, true);
  }
}

// Keep the server's push address for this phone the one the phone really has, on every refresh:
// iOS can replace or drop a subscription (after an update, or the app being re-added), and the
// push service may still accept messages for the old one that never show up.
let healing = false;
async function healSubscription() {
  if (healing || !pushSupported || !registration) return;
  healing = true;
  try {
    if (Notification.permission === 'denied') {  // turned off in Settings: stop counting on this phone
      if (status.device.subscribed) {
        await api('DELETE', '/subscription');
        status.device.subscribed = false;
        renderStatus();
      }
      return;
    }
    if (Notification.permission !== 'granted') return;
    let sub = await registration.pushManager.getSubscription();
    if (!sub && status.device.subscribed) {  // the phone dropped it: make a new one (already allowed)
      sub = await registration.pushManager.subscribe({
        userVisibleOnly: true,
        applicationServerKey: urlBase64ToUint8Array(status.vapidPublicKey),
      });
    }
    if (sub && (!status.device.subscribed || !sub.endpoint.endsWith(status.device.endpointTail || '\u0000'))) {
      await api('POST', '/subscribe', { subscription: sub.toJSON() });
      status.device.subscribed = true;
      status.device.endpointTail = sub.endpoint.slice(-24);
      renderStatus();
    }
  } catch {
    // try again on the next refresh
  } finally {
    healing = false;
  }
}

function urlBase64ToUint8Array(base64) {
  const padded = (base64 + '='.repeat((4 - (base64.length % 4)) % 4)).replace(/-/g, '+').replace(/_/g, '/');
  return Uint8Array.from(atob(padded), (c) => c.charCodeAt(0));
}

// Must run directly from a tap: iOS only allows permission prompts from user gestures.
async function enableNotifications() {
  message('');
  const wasOn = pushSupported && Notification.permission === 'granted' && status && status.device.subscribed;
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
    await api('POST', '/subscribe', { subscription: sub.toJSON() });  // also repairs a lost subscription
    message(wasOn ? 'Notifications are already on for this iPhone.' : 'Notifications are on.');
    await refresh();
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

async function setPolicy() {
  const cap = (id) => ($(id).value ? Number($(id).value) : null);
  const body = { cap_five_hour: cap('cap-five_hour'), cap_weekly: cap('cap-weekly'), allow: $('owner-allow').checked };
  try {
    const { policy } = await api('PUT', '/owner/policy', body);
    status.policy = policy;
    message(policy.allow ? `${status.userName} has been notified.` : `${status.userName} was asked to stop.`);
  } catch (err) {
    message(err.message, true);
  }
  renderStatus();
}

// ---- Widgets (Scriptable) ----------------------------------------------------------------
// One tap: a new read-only widget key, wrapped in a short script for Scriptable that fetches the
// widgets themselves (widget.js) from here every time, so they update without pasting again.
let widgetScript = '';

function widgetScriptFor(key) {
  return [
    '// Claude Usage widgets for Scriptable, made by the Claude Usage app.',
    '// Widget parameter: an account\'s name, "all", or empty for the one in use.',
    `const CLAUDE_USAGE = { url: ${JSON.stringify(location.origin)}, key: ${JSON.stringify(key)} };`,
    'const fm = FileManager.local();',
    "const saved = fm.joinPath(fm.documentsDirectory(), 'claude-usage-widget-code.js');",
    'let code = null;',
    'try {',
    "  const r = new Request(CLAUDE_USAGE.url + '/widget.js?t=' + Date.now());",
    '  r.timeoutInterval = 8;',
    '  const fresh = await r.loadString();',
    "  if (r.response.statusCode === 200 && fresh.includes('Claude Usage widgets')) { code = fresh; fm.writeString(saved, fresh); }",
    '} catch (e) {}',
    'if (!code && fm.fileExists(saved)) code = fm.readString(saved);  // offline: the last copy',
    "if (!code) throw new Error('Could not load the widgets. Check the connection and run again.');",
    'await new (Object.getPrototypeOf(async function () {}).constructor)(\'CLAUDE_USAGE\', code)(CLAUDE_USAGE);',
    '',
  ].join('\n');
}

// The previews show what this phone's widgets will: every account for the user, one for a friend.
function renderWidgetPreviews() {
  const one = isOwnerView();
  for (const img of document.querySelectorAll('.widget-previews img')) {
    const name = img.dataset.kind || (img.dataset.kind = img.getAttribute('src').replace(/^\/?widgets\/|\.png.*$/g, ''));  // (absolute paths: a friend's link lives at /join/...)
    const src = `/widgets/${one ? 'one-' : ''}${name}.png?v=6`;
    if (img.getAttribute('src') !== src) img.setAttribute('src', src);
  }
}

function showWidgetSteps(copied) {
  $('widget-key-out').hidden = false;
  $('widget-copied').textContent = copied ? 'The widget script is copied.' : 'Copy the script below (touch and hold, Select All, Copy).';
  $('widget-script').hidden = copied;
  $('widget-script').value = widgetScript;
  const names = (status && status.accounts ? status.accounts.map((a) => a.name) : []).join(' and ');
  $('widget-param-step').hidden = isOwnerView() || !names;
  $('widget-names').textContent = names;
}

async function createWidgetKey() {
  if (status && status.device.hasWidgetKey &&
      !confirm('Make a new widget script? Widgets set up with the old one stop until you paste the new one.')) return;
  // Safari only lets a tap copy, so the copy starts now and is filled in when the key arrives.
  const script = api('POST', '/widget-key').then(({ key }) => {
    widgetScript = widgetScriptFor(key);
    if (status) status.device.hasWidgetKey = true;
    return widgetScript;
  });
  let copied = false;
  try {
    if (window.ClipboardItem && navigator.clipboard.write) {
      await navigator.clipboard.write([new ClipboardItem({ 'text/plain': script.then((s) => new Blob([s], { type: 'text/plain' })) })]);
    } else {
      await navigator.clipboard.writeText(await script);
    }
    copied = true;
  } catch {}
  try {
    await script;
    showWidgetSteps(copied);
  } catch (err) {
    message(err.message, true);
  }
}

async function copyWidgetKey() {
  try {
    await navigator.clipboard.writeText(widgetScript);
    $('widget-key-copy').textContent = 'Copied';
  } catch {
    showWidgetSteps(false);
  }
}

// First tap: the red warning. Only its own "Unpair" button goes ahead.
function askUnpair() {
  $('unpair-confirm').hidden = false;
  $('unpair').hidden = true;
  $('unpair-confirm').scrollIntoView({ behavior: 'smooth', block: 'center' });
}

function cancelUnpair() {
  $('unpair-confirm').hidden = true;
  $('unpair').hidden = false;
}

async function unpair() {
  cancelUnpair();
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

async function pairWith(code) {
  const err = $('pair-error');
  err.hidden = true;
  try {
    const { token: newToken } = await api('POST', '/pair', { code, name: 'iPhone' });
    token = newToken;
    localStorage.setItem(TOKEN_KEY, token);
    $('pair-code').value = '';
    startDashboard();
  } catch (e) {
    show('pair');
    if (joinCode && code === joinCode) {
      localStorage.setItem(FAILED_JOIN_KEY, joinCode);  // don't retry it on every open
      err.textContent = 'This link was already used or has expired. Ask for a new one.';
    } else {
      err.textContent = e.message;
    }
    err.hidden = false;
  }
}

function pair(event) {
  event.preventDefault();
  pairWith($('pair-code').value);
}

// A newer version on the server: load it (at most once per version, in case of a stale cache).
async function checkForUpdate() {
  try {
    const res = await fetch(location.pathname, { cache: 'no-store' });
    const m = /name="app-version" content="([^"]+)"/.exec(await res.text());
    const key = `claude-usage.reloaded-for-${m && m[1]}`;
    if (m && APP_VERSION && m[1] !== APP_VERSION && !sessionStorage.getItem(key)) {
      sessionStorage.setItem(key, '1');
      location.reload();
    }
  } catch {
    // offline: try again next time
  }
}

async function startDashboard() {
  show('dashboard');
  await refresh();
}

// ---- boot ------------------------------------------------------------------

$('pair-form').addEventListener('submit', pair);
$('enable').addEventListener('click', enableNotifications);
// Where the system allows it (Android, installed): keep it upright. iOS shows the cover instead.
try { if (screen.orientation && screen.orientation.lock) screen.orientation.lock('portrait').catch(() => {}); } catch { /* not allowed */ }
// A test to this phone, sent 10 seconds later so it can be locked first (as real ones arrive).
$('test-push').addEventListener('click', async () => {
  try {
    await api('POST', '/test');
    message('Test sent: lock your iPhone now, it arrives in about 10 seconds.');
  } catch (err) {
    message(err.message, true);
  }
});
$('notify-banner-btn').addEventListener('click', enableNotifications);
// iOS still pinch-zooms some pages despite the viewport setting: stop the gesture itself.
for (const type of ['gesturestart', 'gesturechange']) document.addEventListener(type, (e) => e.preventDefault(), { passive: false });
$('refresh').addEventListener('click', refreshNow);
$('unpair').addEventListener('click', askUnpair);
$('auto-cancel').addEventListener('click', () => { $('auto-confirm').hidden = true; });
$('auto-yes').addEventListener('click', () => {
  if (autoFor === 'owner') setOwnerAutoCap(true);
  else if (autoFor) setAutoCap(autoFor, true);
});
$('unpair-cancel').addEventListener('click', cancelUnpair);
$('unpair-yes').addEventListener('click', unpair);
$('widget-key').addEventListener('click', createWidgetKey);
$('reserve-btn').addEventListener('click', toggleReserve);
// Opened from "Let Mazen use your 40%?": bring the button into view once the page is drawn.
if (new URLSearchParams(location.search).has('reserve')) {
  history.replaceState(null, '', '/');
  const showReserve = () => {
    if ($('reserve-box').hidden) return setTimeout(showReserve, 400);
    $('reserve-box').scrollIntoView({ behavior: 'smooth', block: 'center' });
    $('reserve-box').classList.add('flash');
  };
  setTimeout(showReserve, 400);
}
$('widget-key-copy').addEventListener('click', copyWidgetKey);
for (const kind of [...PREFS.user, ...PREFS.owner]) {
  $(`pref-${kind}`).addEventListener('change', (e) => setPref(kind, e.target.checked));
}
for (const id of ['owner-allow', 'cap-five_hour', 'cap-weekly']) $(id).addEventListener('change', setPolicy);

if ('serviceWorker' in navigator) {
  navigator.serviceWorker.register('/sw.js', { scope: '/' }).then((r) => {
    registration = r;
  }, () => {});
}

$('install-auto').hidden = !joinCode;
if (isIOS && !isStandalone) show('install');
// A personal link signs in by itself only in the installed app: opening it in a desktop browser
// (or a chat app's preview) must not use it up. Elsewhere the code is filled in, one tap away.
else if (!token && joinCode && isStandalone && localStorage.getItem(FAILED_JOIN_KEY) !== joinCode) pairWith(joinCode);
else if (!token) {
  show('pair');
  if (joinCode) $('pair-code').value = joinCode;
}
else startDashboard();
checkForUpdate();

// Countdowns tick locally; the server is asked for fresh numbers every 30 s while visible.
setInterval(() => status && !document.hidden && renderStatus(), 1000);
setInterval(() => token && !document.hidden && refresh(), 5_000);  // the PC sends each reading at once
setInterval(pollTokens, 2_000);  // the live tokens card, while it is on screen
document.addEventListener('visibilitychange', () => {
  if (document.hidden) return;
  checkForUpdate();
  if (token) refresh();
});
