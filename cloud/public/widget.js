// Claude Usage widgets for Scriptable (iOS).
//   Home Screen, Today View and StandBy: small, medium, large.
//   Lock Screen: circular, rectangular, inline.
// Rings like Apple's Activity: the outer ring is the 5-hour limit, the inner ring the week.
//
// Set up from the phone app (Widgets > Set Up Widgets): it copies a short script holding your
// cloud address and a read-only widget key, which fetches this file each time, so the widgets
// update themselves. (Pasting this file directly also works: run it once to enter the two.)
//
// Whose: the PC's owner sees every account together (up to three, on every widget, the Lock Screen
// too); a friend's widgets show only their own account.
// Each account: its 5-hour and weekly bars, and when each resets.
// Style: usage bars, as on the taskbar and in the app (quarters, and the cap as a bright line).
// An account's name as the widget's parameter (long-press > Edit Widget > Parameter) pins it to
// that account.

const KEYCHAIN = 'claude-usage-widget';
const CACHE = 'claude-usage-widget.json';

const P = {
  bg: new Color('#000000'),           // Apple's widgets sit on true black
  tile: new Color('#1c1c1e'),
  text: Color.white(),
  muted: new Color('#8e8e93'),
  faint: new Color('#48484a'),
  five: new Color('#ff7a4d'),         // Claude orange
  fiveTrack: new Color('#ff7a4d', 0.22),
  week: new Color('#64d2ff'),         // Apple cyan
  weekTrack: new Color('#64d2ff', 0.22),
  critical: new Color('#ff453a'),
  on: new Color('#30d158'),
  off: new Color('#ff453a'),
};

// ---- config and data -------------------------------------------------------------------

async function askConfig() {
  const a = new Alert();
  a.title = 'Claude Usage widget';
  a.message = 'Your cloud address and the widget key from the phone app (Widgets).';
  a.addTextField('https://claude-usage-cloud.you.workers.dev', '');
  a.addTextField('Widget key', '');
  a.addAction('Save');
  a.addCancelAction('Cancel');
  if ((await a.present()) === -1) return null;
  const url = a.textFieldValue(0).trim().replace(/\/+$/, '');
  const key = a.textFieldValue(1).trim();
  if (!/^https:\/\//.test(url) || key.length < 20) throw new Error('Enter an https:// address and the widget key.');
  const cfg = { url, key };
  Keychain.set(KEYCHAIN, JSON.stringify(cfg));
  return cfg;
}

function savedConfig() {
  // The phone app's setup script passes CLAUDE_USAGE; a pasted copy of this file keeps it in the Keychain.
  if (typeof CLAUDE_USAGE !== 'undefined' && CLAUDE_USAGE && CLAUDE_USAGE.url) return CLAUDE_USAGE;
  return Keychain.contains(KEYCHAIN) ? JSON.parse(Keychain.get(KEYCHAIN)) : null;
}

const fm = FileManager.local();
const cachePath = fm.joinPath(fm.cacheDirectory(), CACHE);

async function loadData(cfg) {
  try {
    const req = new Request(`${cfg.url}/api/widget`);
    req.headers = { Authorization: `Bearer ${cfg.key}` };
    req.timeoutInterval = 10;
    const data = await req.loadJSON();
    if (req.response.statusCode !== 200) throw new Error(data.error || `HTTP ${req.response.statusCode}`);
    fm.writeString(cachePath, JSON.stringify(data));
    return { data, offline: false };
  } catch (err) {
    if (fm.fileExists(cachePath)) return { data: JSON.parse(fm.readString(cachePath)), offline: true };
    throw err;
  }
}

// Every account the key may see, the one in use first. Older servers only send `usage`.
function accountsOf(data) {
  let list = Array.isArray(data.accounts) ? data.accounts.slice() : [];
  if (!list.length && data.usage) {
    list = [{ name: 'Claude', active: true, five_hour: data.usage.five_hour, seven_day: data.usage.seven_day }];
  }
  return list.sort((a, b) => (b.active ? 1 : 0) - (a.active ? 1 : 0));
}

// The widget's parameter, in words: "bars" (or "rings") for the style, anything else an account.
const PARAM = String(args.widgetParameter || '').toLowerCase().split(/[\s,]+/).filter(Boolean);
const BARS = true;  // every widget is bars now (the ring layouts are kept below, unused)

// Every account together, unless the parameter names one of them.
function pickAccounts(data) {
  const list = accountsOf(data);
  const one = list.find((a) => PARAM.includes(String(a.name).toLowerCase()));
  return one ? [one] : list;
}

const initial = (acc) => String(acc.name).slice(0, 1).toLowerCase();

// The shortest start of each name that tells the accounts apart (alice, bob, carol: h, na, no).
function shortNames(accounts) {
  const names = accounts.map((a) => String(a.name).toLowerCase());
  return names.map((n, i) => {
    for (let k = 1; k < n.length; k++) {
      if (!names.some((m, j) => j !== i && m.startsWith(n.slice(0, k)))) return n.slice(0, k);
    }
    return n;
  });
}

// ---- numbers ----------------------------------------------------------------------------

const nowS = () => Date.now() / 1000;

// A limit as shown: a window whose reset has passed starts again at 0.
function windowOf(w) {
  if (!w) return null;
  const passed = w.resetsAt > 0 && w.resetsAt <= nowS();
  return {
    pct: passed ? 0 : Math.max(0, Math.min(100, w.percent)),
    left: passed ? 0 : w.resetsAt > 0 ? w.resetsAt - nowS() : null,
  };
}

function duration(seconds) {
  if (seconds === null || seconds === undefined) return '--';
  if (seconds <= 0) return 'now';
  if (seconds < 60) return '<1m';
  const m = Math.floor(seconds / 60);
  if (m < 60) return `${m}m`;
  const h = Math.floor(m / 60);
  if (h < 24) return m % 60 ? `${h}h ${m % 60}m` : `${h}h`;
  return h % 24 ? `${Math.floor(h / 24)}d ${h % 24}h` : `${Math.floor(h / 24)}d`;
}

// Shorter, for tight spots: 3h45m, 2d5h; and shortest: 45m, 3h, 2d.
const compact = (seconds) => duration(seconds).replace(' ', '');
function tiny(seconds) {
  if (seconds === null || seconds === undefined) return '--';
  if (seconds <= 0) return 'now';
  const m = Math.floor(seconds / 60);
  if (m < 60) return `${Math.max(1, m)}m`;
  const h = Math.floor(m / 60);
  return h < 24 ? `${h}h` : `${Math.floor(h / 24)}d`;
}
const resetOf = (l, fmt) => (l && l.left !== null ? fmt(l.left) : '--');

const pctText = (l) => (l ? `${Math.round(l.pct)}%` : '--');
const valueColor = (l, ring) => (l && l.pct >= 95 ? P.critical : ring);

// ---- drawing ----------------------------------------------------------------------------

// Concentric rings, outermost first: [{ pct, color, track }]. Each arc is laid down as a run of
// small discs, which gives it Apple's rounded ends.
function ringsImage(size, rings, width, gap) {
  const ctx = new DrawContext();
  ctx.size = new Size(size, size);
  ctx.opaque = false;
  ctx.respectScreenScale = true;
  const c = size / 2;
  rings.forEach((ring, i) => {
    const r = c - width / 2 - i * (width + gap);
    if (r <= width / 2) return;
    ctx.setStrokeColor(ring.track);
    ctx.setLineWidth(width);
    ctx.strokeEllipse(new Rect(c - r, c - r, 2 * r, 2 * r));
    const pct = Math.max(0, Math.min(100, ring.pct || 0));
    if (pct <= 0) return;
    const sweep = (pct / 100) * 2 * Math.PI;
    const steps = Math.max(2, Math.ceil((sweep * r) / (width * 0.18)));
    ctx.setFillColor(ring.color);
    for (let k = 0; k <= steps; k++) {
      const a = -Math.PI / 2 + (sweep * k) / steps;
      ctx.fillEllipse(new Rect(c + r * Math.cos(a) - width / 2, c + r * Math.sin(a) - width / 2, width, width));
    }
  });
  return ctx.getImage();
}

function accountRings(acc, size, width, gap, mono) {
  const five = windowOf(acc.five_hour);
  const week = windowOf(acc.seven_day);
  const white = Color.white();
  return ringsImage(size, [
    { pct: five ? five.pct : 0, color: mono ? white : P.five, track: mono ? new Color('#ffffff', 0.25) : P.fiveTrack },
    { pct: week ? week.pct : 0, color: mono ? white : P.week, track: mono ? new Color('#ffffff', 0.25) : P.weekTrack },
  ], width, gap);
}

function text(stack, value, font, color) {
  const t = stack.addText(value);
  t.font = font;
  if (color) t.textColor = color;
  t.lineLimit = 1;
  t.minimumScaleFactor = 0.6;
  return t;
}

function statusDot(stack, acc) {
  text(stack, '●', Font.systemFont(8), acc.active ? P.on : P.off);
}

// "77%" big in the ring's colour, its name and reset time beside it.
function statRow(stack, label, l, ring, big) {
  const row = stack.addStack();
  row.layoutHorizontally();
  row.bottomAlignContent();
  text(row, pctText(l), Font.boldRoundedSystemFont(big), valueColor(l, ring));
  row.addSpacer(6);
  const side = row.addStack();
  side.layoutVertically();
  text(side, label, Font.semiboldRoundedSystemFont(10), ring);
  text(side, l && l.left !== null ? `resets ${duration(l.left)}` : ' ', Font.mediumRoundedSystemFont(10), P.muted);
  row.addSpacer();
}

function footer(w, data, offline) {
  const age = Math.round((Date.now() - (data.accountsReceivedAt || data.usageReceivedAt || 0)) / 1000);
  const t = data.tokens;
  const live = t && t.rate > 0 && Date.now() - t.at < 60_000;
  const note = offline ? 'offline' : age > 600 ? `updated ${duration(age)} ago` : live ? `${t.rate} tok/s` : '';
  if (!note) return;
  const f = w.addStack();
  f.addSpacer();
  text(f, note, Font.mediumRoundedSystemFont(9), P.muted);
}

// ---- bars --------------------------------------------------------------------------------

// The cap in force on a limit (the user's widgets get the caps): the lower of the two.
function capOf(acc, key) {
  const c = acc.caps;
  if (!c) return null;
  const five = key === 'five_hour';
  if (five && c.reserve) return c.reserve.from;
  const a = five ? c.mine_five_hour : c.mine_weekly;
  const b = five ? c.owner_five_hour : c.owner_weekly;
  return a && b ? Math.min(a, b) : a || b || null;
}

// A rounded bar at y: a faint track, the fill, quarter breaks, and a bright line at the cap that
// reaches `reach` beyond the bar.
function drawBar(ctx, y, width, height, pct, color, track, cap, mono, reach) {
  const r = height / 2;
  const t = new Path();
  t.addRoundedRect(new Rect(0, y, width, height), r, r);
  ctx.addPath(t);
  ctx.setFillColor(track);
  ctx.fillPath();
  if (pct > 0) {
    const f = new Path();
    f.addRoundedRect(new Rect(0, y, Math.max(height, (width * Math.min(100, pct)) / 100), height), r, r);
    ctx.addPath(f);
    ctx.setFillColor(color);
    ctx.fillPath();
  }
  // Quarters: thin breaks at 25, 50 and 75%.
  ctx.setFillColor(mono ? new Color('#000000', 0.55) : P.bg);
  for (const q of [0.25, 0.5, 0.75]) ctx.fillRect(new Rect(Math.round(width * q) - 0.5, y, 1, height));
  if (cap) {
    ctx.setFillColor(Color.white());
    ctx.fillRect(new Rect(Math.round((width * cap) / 100) - 1, y - reach, 2, height + 2 * reach));
  }
}

function barImage(width, height, pct, color, track, cap, mono) {
  const ctx = new DrawContext();
  ctx.size = new Size(width, height + 4);
  ctx.opaque = false;
  ctx.respectScreenScale = true;
  drawBar(ctx, 2, width, height, pct, color, track, cap, mono, 2);
  return ctx.getImage();
}

// An account's two limits in one small Lock Screen image: the 5-hour bar, the week's thinner one
// under it.
function pairImage(width, acc) {
  const ctx = new DrawContext();
  ctx.size = new Size(width, 12);
  ctx.opaque = false;
  ctx.respectScreenScale = true;
  const five = windowOf(acc.five_hour);
  const week = windowOf(acc.seven_day);
  const white = Color.white();
  drawBar(ctx, 1, width, 5, five ? five.pct : 0, white, new Color('#ffffff', 0.25), capOf(acc, 'five_hour'), true, 1);
  drawBar(ctx, 8, width, 3, week ? week.pct : 0, white, new Color('#ffffff', 0.25), capOf(acc, 'seven_day'), true, 1);
  return ctx.getImage();
}

// A fixed-width cell, so the columns line up row to row.
function cell(stack, width, value, font, color) {
  const c = stack.addStack();
  c.size = new Size(width, 0);
  const t = text(c, value, font, color);
  c.addSpacer();
  return t;
}

// "5H  ▬▬▬▬▬░░░  77%": the label, the bar, the percent (and, if wide, when it resets).
function barRow(stack, acc, key, width, opts = {}) {
  const five = key === 'five_hour';
  const l = windowOf(acc[key]);
  const ring = five ? P.five : P.week;
  const row = stack.addStack();
  row.layoutHorizontally();
  row.centerAlignContent();
  const lab = row.addStack();
  lab.size = new Size(opts.labelW || 22, 0);
  text(lab, five ? '5H' : 'WK', Font.semiboldRoundedSystemFont(opts.labelSize || 10), opts.mono ? null : ring);
  row.addSpacer(4);
  const h = opts.barH || 6;
  row.addImage(barImage(width, h, l ? l.pct : 0, opts.mono ? Color.white() : ring,
    opts.mono ? new Color('#ffffff', 0.25) : five ? P.fiveTrack : P.weekTrack, capOf(acc, key), opts.mono)).imageSize = new Size(width, h + 4);
  row.addSpacer(opts.gap === undefined ? 6 : opts.gap);
  const pctFont = Font.boldRoundedSystemFont(opts.pctSize || 12);
  const pctColor = opts.mono ? null : valueColor(l, ring);
  if (opts.pctW) cell(row, opts.pctW, pctText(l), pctFont, pctColor);
  else text(row, pctText(l), pctFont, pctColor);
  if (opts.reset) {
    row.addSpacer(opts.pctW ? 2 : 5);
    text(row, l && l.left !== null ? (opts.compact ? compact : duration)(l.left) : '',
      Font.mediumRoundedSystemFont(opts.resetSize || 10), opts.mono ? null : P.muted);
  }
  return row;
}

function nameRow(stack, acc, size, withState) {
  const head = stack.addStack();
  head.centerAlignContent();
  statusDot(head, acc);
  head.addSpacer(4);
  text(head, acc.name, Font.semiboldRoundedSystemFont(size), P.text);
  if (withState) {
    head.addSpacer(6);
    text(head, acc.active ? 'In use' : 'Not in use', Font.mediumRoundedSystemFont(10), P.muted);
  }
  return head;
}

function smallBars(accounts) {
  const w = new ListWidget();
  w.backgroundColor = P.bg;
  w.setPadding(12, 12, 12, 12);
  if (accounts.length > 1) {
    const shown = accounts.slice(0, 3);
    const tight = shown.length > 2;
    if (tight) w.setPadding(9, 11, 9, 10);
    const o = { reset: true, compact: true, labelW: 16, labelSize: 9, pctSize: 10, pctW: 25, gap: 4, barH: 5 };
    shown.forEach((acc, i) => {
      if (i) w.addSpacer(tight ? 5 : 9);
      nameRow(w, acc, tight ? 11 : 12);
      w.addSpacer(tight ? 1 : 3);
      barRow(w, acc, 'five_hour', 52, o);
      barRow(w, acc, 'seven_day', 52, o);
    });
    return w;
  }
  const acc = accounts[0];
  nameRow(w, acc, 13);
  w.addSpacer();
  const five = windowOf(acc.five_hour);
  text(w, pctText(five), Font.boldRoundedSystemFont(30), valueColor(five, P.five));
  text(w, five && five.left !== null ? `of 5 hours \u00B7 ${duration(five.left)}` : 'of 5 hours', Font.mediumRoundedSystemFont(10), P.muted);
  w.addSpacer();
  const o = { reset: true, compact: true, labelW: 18, pctSize: 11, pctW: 28, gap: 4 };
  barRow(w, acc, 'five_hour', 50, o);
  w.addSpacer(3);
  barRow(w, acc, 'seven_day', 50, o);
  return w;
}

function mediumBars(accounts, data, offline) {
  const w = new ListWidget();
  w.backgroundColor = P.bg;
  w.setPadding(14, 16, 10, 16);
  w.addSpacer();
  const one = accounts.length === 1;  // one account: bigger bars and numbers
  if (!one) {
    const shown = accounts.slice(0, 3);
    const o = { reset: true, labelW: 20, pctW: 32, gap: 5, barH: shown.length > 2 ? 6 : 7 };
    shown.forEach((acc, i) => {
      if (i) w.addSpacer(shown.length > 2 ? 9 : 14);
      const row = w.addStack();
      row.centerAlignContent();
      const who = row.addStack();
      who.size = new Size(64, 0);
      who.layoutVertically();
      nameRow(who, acc, 13);
      text(who, acc.active ? 'In use' : 'Not in use', Font.mediumRoundedSystemFont(9), P.muted);
      const bars = row.addStack();
      bars.layoutVertically();
      barRow(bars, acc, 'five_hour', 130, o);
      bars.addSpacer(2);
      barRow(bars, acc, 'seven_day', 130, o);
    });
    w.addSpacer();
    footer(w, data, offline);
    return w;
  }
  const big = { reset: true, barH: 10, pctSize: 17, labelW: 26, labelSize: 12 };
  accounts.slice(0, 2).forEach((acc, i) => {
    if (i) w.addSpacer(10);
    nameRow(w, acc, one ? 15 : 13, true);
    w.addSpacer(one ? 10 : 5);
    barRow(w, acc, 'five_hour', one ? 160 : 170, big);
    w.addSpacer(one ? 8 : 3);
    barRow(w, acc, 'seven_day', one ? 160 : 170, big);
  });
  w.addSpacer();
  footer(w, data, offline);
  return w;
}

function largeBars(accounts, data, offline) {
  const w = new ListWidget();
  w.backgroundColor = P.bg;
  w.setPadding(16, 16, 12, 16);
  const head = w.addStack();
  head.centerAlignContent();
  text(head, 'Claude', Font.boldRoundedSystemFont(17), P.five);
  text(head, ' Usage', Font.boldRoundedSystemFont(17), P.text);
  head.addSpacer();
  const t = data.tokens;
  if (t && Date.now() - t.at < 60_000) text(head, `${t.rate} tok/s`, Font.semiboldRoundedSystemFont(12), P.muted);
  accounts.slice(0, 3).forEach((acc) => {
    w.addSpacer();
    nameRow(w, acc, 15, true);
    w.addSpacer(8);
    barRow(w, acc, 'five_hour', 180, { reset: true, barH: 9, pctSize: 15, labelW: 26, labelSize: 11 });
    w.addSpacer(6);
    barRow(w, acc, 'seven_day', 180, { reset: true, barH: 9, pctSize: 15, labelW: 26, labelSize: 11 });
  });
  w.addSpacer();
  footer(w, data, offline);
  return w;
}

// Lock Screen (tinted by iOS, so drawn in white). Several accounts: a row each - the name, the
// 5-hour bar and its reset, the week's bar and its reset. One account: its two bars, with resets.
function lockBars(accounts) {
  const w = new ListWidget();
  if (accounts.length > 1) {
    const shown = accounts.slice(0, 3);
    const nameW = 40, barW = 34, resetW = 17;
    const head = w.addStack();
    head.addSpacer(nameW);
    cell(head, barW + 2 + resetW + 6, '5 hours', Font.semiboldRoundedSystemFont(9));
    cell(head, barW + 2 + resetW, 'week', Font.semiboldRoundedSystemFont(9));
    shown.forEach((acc) => {
      w.addSpacer(shown.length > 2 ? 2 : 4);
      const row = w.addStack();
      row.centerAlignContent();
      cell(row, nameW, acc.name, Font.boldRoundedSystemFont(11));
      ['five_hour', 'seven_day'].forEach((key, k) => {
        if (k) row.addSpacer(6);
        const l = windowOf(acc[key]);
        row.addImage(barImage(barW, 5, l ? l.pct : 0, Color.white(), new Color('#ffffff', 0.25), capOf(acc, key), true)).imageSize = new Size(barW, 9);
        row.addSpacer(2);
        cell(row, resetW, resetOf(l, tiny), Font.boldRoundedSystemFont(10));
      });
    });
    return w;
  }
  const acc = accounts[0];
  const mono = { mono: true, labelW: 18, pctSize: 12, pctW: 30, gap: 4, reset: true, compact: true, resetSize: 11 };
  text(w, acc.name, Font.boldRoundedSystemFont(13));
  w.addSpacer(2);
  barRow(w, acc, 'five_hour', 56, mono);
  w.addSpacer(1);
  barRow(w, acc, 'seven_day', 56, mono);
  return w;
}

// ---- Home Screen ------------------------------------------------------------------------

function small(acc) {
  const w = new ListWidget();
  w.backgroundColor = P.bg;
  w.setPadding(14, 14, 12, 12);
  const top = w.addStack();
  top.layoutHorizontally();
  top.topAlignContent();
  top.addImage(accountRings(acc, 62, 8, 2)).imageSize = new Size(62, 62);
  top.addSpacer();
  const who = top.addStack();
  who.layoutVertically();
  const name = who.addStack();
  name.centerAlignContent();
  statusDot(name, acc);
  name.addSpacer(3);
  text(name, acc.name, Font.semiboldRoundedSystemFont(12), P.text);
  w.addSpacer();
  statRow(w, '5-HOUR', windowOf(acc.five_hour), P.five, 20);
  w.addSpacer(3);
  statRow(w, 'WEEK', windowOf(acc.seven_day), P.week, 20);
  return w;
}

// Two accounts side by side: each its own rings, name, and both limits.
function smallMulti(accounts) {
  const w = new ListWidget();
  w.backgroundColor = P.bg;
  w.setPadding(12, 10, 10, 10);
  w.addSpacer();
  const row = w.addStack();
  row.layoutHorizontally();
  row.topAlignContent();
  accounts.slice(0, 2).forEach((acc, i) => {
    if (i) row.addSpacer();
    const col = row.addStack();
    col.layoutVertically();
    col.addImage(accountRings(acc, 62, 8, 2)).imageSize = new Size(62, 62);
    col.addSpacer(6);
    const name = col.addStack();
    name.centerAlignContent();
    statusDot(name, acc);
    name.addSpacer(3);
    text(name, acc.name, Font.semiboldRoundedSystemFont(12), P.text);
    const five = windowOf(acc.five_hour);
    const week = windowOf(acc.seven_day);
    const nums = col.addStack();
    nums.bottomAlignContent();
    text(nums, pctText(five), Font.boldRoundedSystemFont(15), valueColor(five, P.five));
    nums.addSpacer(3);
    text(nums, pctText(week), Font.boldRoundedSystemFont(11), valueColor(week, P.week));
    text(col, five && five.left !== null ? duration(five.left) : ' ', Font.mediumRoundedSystemFont(9), P.muted);
  });
  w.addSpacer();
  return w;
}

function accountBlock(parent, acc, ringSize, big) {
  const col = parent.addStack();
  col.layoutVertically();
  const head = col.addStack();
  head.centerAlignContent();
  statusDot(head, acc);
  head.addSpacer(4);
  text(head, acc.name, Font.semiboldRoundedSystemFont(14), P.text);
  head.addSpacer(6);
  text(head, acc.active ? 'In use' : 'Not in use', Font.mediumRoundedSystemFont(10), P.muted);
  col.addSpacer(6);
  statRow(col, '5-HOUR', windowOf(acc.five_hour), P.five, big);
  col.addSpacer(4);
  statRow(col, 'WEEK', windowOf(acc.seven_day), P.week, big);
}

function medium(accounts, data, offline) {
  const w = new ListWidget();
  w.backgroundColor = P.bg;
  w.setPadding(14, 16, 10, 16);
  w.addSpacer();
  const row = w.addStack();
  row.layoutHorizontally();
  row.centerAlignContent();
  if (accounts.length > 1) {  // two accounts side by side, rings above each
    accounts.slice(0, 2).forEach((acc) => {
      row.addSpacer();  // the two accounts spread evenly across the widget
      const col = row.addStack();
      col.layoutHorizontally();
      col.centerAlignContent();
      col.addImage(accountRings(acc, 58, 7, 2)).imageSize = new Size(58, 58);
      col.addSpacer(8);
      const info = col.addStack();
      info.layoutVertically();
      const head = info.addStack();
      head.centerAlignContent();
      statusDot(head, acc);
      head.addSpacer(3);
      text(head, acc.name, Font.semiboldRoundedSystemFont(13), P.text);
      info.addSpacer(3);
      const five = windowOf(acc.five_hour);
      const week = windowOf(acc.seven_day);
      text(info, pctText(five), Font.boldRoundedSystemFont(18), valueColor(five, P.five));
      text(info, five && five.left !== null ? duration(five.left) : ' ', Font.mediumRoundedSystemFont(9), P.muted);
      text(info, pctText(week), Font.boldRoundedSystemFont(18), valueColor(week, P.week));
    });
    row.addSpacer();
  } else if (accounts.length) {
    row.addImage(accountRings(accounts[0], 118, 14, 3)).imageSize = new Size(118, 118);
    row.addSpacer(16);
    accountBlock(row, accounts[0], 118, 26);
  }
  w.addSpacer();
  footer(w, data, offline);
  return w;
}

function large(accounts, data, offline) {
  const w = new ListWidget();
  w.backgroundColor = P.bg;
  w.setPadding(16, 16, 12, 16);
  const head = w.addStack();
  head.centerAlignContent();
  text(head, 'Claude', Font.boldRoundedSystemFont(17), P.five);
  text(head, ' Usage', Font.boldRoundedSystemFont(17), P.text);
  head.addSpacer();
  const t = data.tokens;
  if (t && Date.now() - t.at < 60_000) text(head, `${t.rate} tok/s`, Font.semiboldRoundedSystemFont(12), P.muted);
  const shown = accounts.slice(0, 3);
  const ring = shown.length > 2 ? 78 : 104;  // rows spread evenly down the widget
  shown.forEach((acc, i) => {
    w.addSpacer();
    if (i) {
      const line = w.addStack();
      line.backgroundColor = P.tile;
      line.size = new Size(0, 1);
      line.addSpacer();
      w.addSpacer();
    }
    const row = w.addStack();
    row.layoutHorizontally();
    row.centerAlignContent();
    row.addImage(accountRings(acc, ring, ring / 7.5, 3)).imageSize = new Size(ring, ring);
    row.addSpacer(16);
    accountBlock(row, acc, ring, 24);
  });
  w.addSpacer();
  footer(w, data, offline);
  return w;
}

// ---- Lock Screen (iOS tints these itself, so they are drawn in white) ---------------------

function lockCircular(acc) {
  const w = new ListWidget();
  w.addAccessoryWidgetBackground = true;
  const s = w.addStack();
  s.size = new Size(64, 64);
  s.backgroundImage = accountRings(acc, 64, 6, 2, true);
  s.centerAlignContent();
  const five = windowOf(acc.five_hour);
  const t = text(s, five ? `${Math.round(five.pct)}` : '--', Font.boldRoundedSystemFont(17));
  t.centerAlignText();
  return w;
}

function lockRectangular(acc) {
  const w = new ListWidget();
  const row = w.addStack();
  row.layoutHorizontally();
  row.centerAlignContent();
  row.addImage(accountRings(acc, 50, 6, 2, true)).imageSize = new Size(50, 50);
  row.addSpacer(8);
  const col = row.addStack();
  col.layoutVertically();
  text(col, acc.name, Font.boldRoundedSystemFont(14));
  const five = windowOf(acc.five_hour);
  const week = windowOf(acc.seven_day);
  text(col, `5h ${pctText(five)} · ${five ? duration(five.left) : '--'}`, Font.mediumRoundedSystemFont(12));
  text(col, `Wk ${pctText(week)} · ${week ? duration(week.left) : '--'}`, Font.mediumRoundedSystemFont(12));
  return w;
}

// The round Lock Screen widget, as bars. Several accounts: a row each - a short name, the 5-hour
// bar with the week's thinner bar under it, and when the 5-hour resets. One account: its 5-hour and
// weekly bars, each with when it resets.
function lockCircularBars(accounts) {
  const w = new ListWidget();
  w.addAccessoryWidgetBackground = true;
  w.setPadding(4, 4, 4, 4);
  const multi = accounts.length > 1;
  const shown = accounts.slice(0, 3);
  const names = shortNames(shown);
  const labelW = Math.max(...names.map((n) => n.length)) > 1 ? 12 : 8;
  w.addSpacer();
  if (multi) {
    shown.forEach((acc, i) => {
      if (i) w.addSpacer(2);
      const row = w.addStack();
      row.centerAlignContent();
      row.addSpacer();
      cell(row, labelW, names[i], Font.boldRoundedSystemFont(9));
      row.addImage(pairImage(26, acc)).imageSize = new Size(26, 12);
      row.addSpacer(2);
      cell(row, 13, resetOf(windowOf(acc.five_hour), tiny), Font.boldRoundedSystemFont(8));
      row.addSpacer();
    });
  } else {
    const acc = accounts[0];
    [['5h', 'five_hour'], ['wk', 'seven_day']].forEach(([label, key], i) => {
      if (i) w.addSpacer(4);
      const l = windowOf(acc[key]);
      const row = w.addStack();
      row.centerAlignContent();
      row.addSpacer();
      cell(row, 13, label, Font.boldRoundedSystemFont(9));
      row.addImage(barImage(24, 5, l ? l.pct : 0, Color.white(), new Color('#ffffff', 0.25), capOf(acc, key), true)).imageSize = new Size(24, 9);
      row.addSpacer(2);
      cell(row, 13, resetOf(l, tiny), Font.boldRoundedSystemFont(9));
      row.addSpacer();
    });
  }
  w.addSpacer();
  return w;
}

// One gauge for two accounts: the outer ring is the one in use, the inner the other (5-hour).
function lockCircularMulti(accounts) {
  const w = new ListWidget();
  w.addAccessoryWidgetBackground = true;
  const [a, b] = accounts;
  const fa = windowOf(a.five_hour);
  const fb = windowOf(b.five_hour);
  const s = w.addStack();
  s.size = new Size(64, 64);
  s.backgroundImage = ringsImage(64, [
    { pct: fa ? fa.pct : 0, color: Color.white(), track: new Color('#ffffff', 0.25) },
    { pct: fb ? fb.pct : 0, color: new Color('#ffffff', 0.75), track: new Color('#ffffff', 0.18) },
  ], 5, 2);
  s.layoutVertically();
  s.centerAlignContent();
  for (const [acc, l, size] of [[a, fa, 11], [b, fb, 10]]) {
    const line = s.addStack();
    line.addSpacer();
    text(line, `${initial(acc)}${l ? Math.round(l.pct) : '--'}`, Font.boldRoundedSystemFont(size));
    line.addSpacer();
  }
  return w;
}

// A row per account: its rings, name, then the 5-hour and the week.
function lockRectangularMulti(accounts) {
  const w = new ListWidget();
  accounts.slice(0, 2).forEach((acc, i) => {
    if (i) w.addSpacer(4);
    const row = w.addStack();
    row.layoutHorizontally();
    row.centerAlignContent();
    row.addImage(accountRings(acc, 30, 4, 1.5, true)).imageSize = new Size(30, 30);
    row.addSpacer(6);
    const five = windowOf(acc.five_hour);
    const week = windowOf(acc.seven_day);
    text(row, acc.name, Font.boldRoundedSystemFont(13));
    row.addSpacer(5);
    text(row, `${pctText(five)} \u00B7 ${pctText(week)}`, Font.mediumRoundedSystemFont(12));
  });
  return w;
}

function lockInline(acc) {
  const w = new ListWidget();
  const five = windowOf(acc.five_hour);
  w.addText(`${acc.name} ${pctText(five)} · resets ${five ? duration(five.left) : '--'}`);
  return w;
}

function messageWidget(message) {
  const w = new ListWidget();
  w.backgroundColor = P.bg;
  text(w, 'Claude Usage', Font.boldRoundedSystemFont(13), P.five);
  w.addSpacer(4);
  const t = w.addText(message);
  t.font = Font.mediumRoundedSystemFont(11);
  t.textColor = P.muted;
  return w;
}

function build(family, data, offline) {
  const accounts = pickAccounts(data);
  if (!accounts.length) return messageWidget('No usage yet: is the PC app running?');
  const multi = accounts.length > 1;
  if (family === 'accessoryCircular') return BARS ? lockCircularBars(accounts) : multi ? lockCircularMulti(accounts) : lockCircular(accounts[0]);
  if (family === 'accessoryRectangular') {
    if (BARS) return lockBars(accounts);
    return multi ? lockRectangularMulti(accounts) : lockRectangular(accounts[0]);
  }
  if (family === 'accessoryInline') {
    const w = new ListWidget();
    if (!multi) {
      const five = windowOf(accounts[0].five_hour);
      const week = windowOf(accounts[0].seven_day);
      w.addText(`${pctText(five)} ${resetOf(five, compact)} \u00B7 wk ${pctText(week)} ${resetOf(week, tiny)}`);
      return w;
    }
    // "h 77/39 · na 42/61 · no 8/12": each account's 5 hours / week.
    const shown = accounts.slice(0, 3);
    const names = shortNames(shown);
    w.addText(shown.map((a, i) => {
      const five = windowOf(a.five_hour);
      const week = windowOf(a.seven_day);
      return `${names[i]} ${five ? Math.round(five.pct) : '--'}/${week ? Math.round(week.pct) : '--'}`;
    }).join(' \u00B7 '));
    return w;
  }
  if (BARS) {
    if (family === 'small') return smallBars(accounts);
    if (family === 'large' || family === 'extraLarge') return largeBars(accounts, data, offline);
    return mediumBars(accounts, data, offline);
  }
  if (family === 'small') return multi ? smallMulti(accounts) : small(accounts[0]);
  if (family === 'large' || family === 'extraLarge') return large(accounts, data, offline);
  return medium(accounts, data, offline);
}

// Refresh again just after the next reset, so the rings empty on time.
function nextRefresh(data) {
  const resets = [];
  for (const a of accountsOf(data))
    for (const k of ['five_hour', 'seven_day']) if (a[k] && a[k].resetsAt > nowS()) resets.push(a[k].resetsAt);
  return new Date(Math.min(Date.now() + 10 * 60_000, ...resets.map((t) => (t + 60) * 1000)));
}

// ---- main ------------------------------------------------------------------------------

async function main() {
  let cfg = savedConfig();
  if (!config.runsInWidget && args.queryParameters && args.queryParameters.reset) cfg = null;
  if (!cfg && !config.runsInWidget) cfg = await askConfig();
  if (!cfg) return { widget: messageWidget('Open this script in Scriptable once to set it up.') };
  const { data, offline } = await loadData(cfg);
  const family = config.widgetFamily || 'medium';
  const widget = build(family, data, offline);
  widget.refreshAfterDate = nextRefresh(data);
  return { widget, data, offline };
}

let result;
try {
  result = await main();
} catch (err) {
  result = { widget: messageWidget(`Unavailable: ${err.message}`) };
}

if (config.runsInWidget) {
  Script.setWidget(result.widget);
} else if (result.data) {
  // In the app: preview any of the widgets.
  const pick = new Alert();
  pick.title = 'Preview a widget';
  const kinds = [['Small', 'small'], ['Medium', 'medium'], ['Large', 'large'],
    ['Lock Screen: circle', 'accessoryCircular'], ['Lock Screen: rectangle', 'accessoryRectangular'],
    ['Lock Screen: one line', 'accessoryInline']];
  for (const [label] of kinds) pick.addAction(label);
  pick.addCancelAction('Done');
  const i = await pick.present();
  if (i >= 0) {
    const family = kinds[i][1];
    const w = build(family, result.data, result.offline);
    const show = { small: 'presentSmall', medium: 'presentMedium', large: 'presentLarge',
      accessoryCircular: 'presentAccessoryCircular', accessoryRectangular: 'presentAccessoryRectangular',
      accessoryInline: 'presentAccessoryInline' }[family];
    await w[show]();
  }
} else {
  await result.widget.presentMedium();
}
Script.complete();
