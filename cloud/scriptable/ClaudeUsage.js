// Claude Usage widget for Scriptable (iOS). Home Screen (small / medium) and
// Lock Screen (rectangular / circular / inline) widgets.
//
// Setup: copy this whole file into a new Scriptable script, run it once inside
// the Scriptable app, and enter your cloud URL and the widget key from the
// Claude Usage phone app (Create Widget Key). Then add a Scriptable widget and
// pick this script. The key is read-only: it can only view usage.

const KEYCHAIN = 'claude-usage-widget';
const CACHE = 'claude-usage-widget.json';
const C = {
  bg: new Color('#141414'),
  text: Color.white(),
  muted: new Color('#9a9a9a'),
  track: new Color('#333333'),
  accent: new Color('#d97757'),
  warn: new Color('#ffb340'),
  critical: new Color('#ff6b6b'),
};

// ---- config ----------------------------------------------------------------------

async function askConfig() {
  const a = new Alert();
  a.title = 'Claude Usage widget';
  a.message = 'Cloud URL (from cloud\\setup.ps1) and the widget key from the phone app.';
  a.addTextField('https://claude-usage-cloud.you.workers.dev', '');
  a.addTextField('Widget key', '');
  a.addAction('Save');
  a.addCancelAction('Cancel');
  if ((await a.present()) === -1) return null;
  const url = a.textFieldValue(0).trim().replace(/\/+$/, '');
  const key = a.textFieldValue(1).trim();
  if (!/^https:\/\//.test(url) || key.length < 20) throw new Error('Enter an https:// URL and the widget key.');
  const cfg = { url, key };
  Keychain.set(KEYCHAIN, JSON.stringify(cfg));
  return cfg;
}

function savedConfig() {
  return Keychain.contains(KEYCHAIN) ? JSON.parse(Keychain.get(KEYCHAIN)) : null;
}

// ---- data ------------------------------------------------------------------------

const fm = FileManager.local();
const cachePath = fm.joinPath(fm.cacheDirectory(), CACHE);

async function loadUsage(cfg) {
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

// ---- formatting ------------------------------------------------------------------

function duration(seconds) {
  if (seconds <= 0) return 'now';
  if (seconds < 60) return '<1m';
  const m = Math.floor(seconds / 60);
  if (m < 60) return `${m}m`;
  const h = Math.floor(m / 60);
  if (h < 24) return m % 60 ? `${h}h ${m % 60}m` : `${h}h`;
  return h % 24 ? `${Math.floor(h / 24)}d ${h % 24}h` : `${Math.floor(h / 24)}d`;
}

const colorFor = (pct) => (pct >= 95 ? C.critical : pct >= 80 ? C.warn : C.text);
const barColorFor = (pct) => (pct >= 95 ? C.critical : pct >= 80 ? C.warn : C.accent);

function limit(data, key) {
  const w = data.usage && data.usage[key];
  if (!w) return null;
  const now = Date.now() / 1000;
  return {
    pct: Math.round(w.percent),
    raw: Math.max(0, Math.min(100, w.percent)),
    reset: w.resetsAt > 0 ? duration(w.resetsAt - now) : '--',
    resetsAt: w.resetsAt,
  };
}

function barImage(width, height, pct, color) {
  const ctx = new DrawContext();
  ctx.size = new Size(width, height);
  ctx.opaque = false;
  ctx.respectScreenScale = true;
  const track = new Path();
  track.addRoundedRect(new Rect(0, 0, width, height), height / 2, height / 2);
  ctx.addPath(track);
  ctx.setFillColor(C.track);
  ctx.fillPath();
  const w = Math.max(height, (width * pct) / 100);
  const fill = new Path();
  fill.addRoundedRect(new Rect(0, 0, w, height), height / 2, height / 2);
  ctx.addPath(fill);
  ctx.setFillColor(color);
  ctx.fillPath();
  return ctx.getImage();
}

function ringImage(size, pct) {
  const ctx = new DrawContext();
  ctx.size = new Size(size, size);
  ctx.opaque = false;
  ctx.respectScreenScale = true;
  const r = size / 2 - 3;
  const c = size / 2;
  ctx.setLineWidth(5);
  for (let i = 0; i < 100; i++) {
    const a1 = (i / 100) * 2 * Math.PI - Math.PI / 2;
    const a2 = ((i + 1) / 100) * 2 * Math.PI - Math.PI / 2;
    const p = new Path();
    p.move(new Point(c + r * Math.cos(a1), c + r * Math.sin(a1)));
    p.addLine(new Point(c + r * Math.cos(a2), c + r * Math.sin(a2)));
    ctx.addPath(p);
    ctx.setStrokeColor(i < pct ? Color.white() : new Color('#ffffff', 0.25));
    ctx.strokePath();
  }
  return ctx.getImage();
}

// ---- widgets ---------------------------------------------------------------------

function addLimitRow(stack, label, l, width, showReset) {
  const row = stack.addStack();
  row.centerAlignContent();
  const name = row.addText(label);
  name.font = Font.mediumSystemFont(13);
  name.textColor = C.muted;
  row.addSpacer();
  const value = row.addText(l ? `${l.pct}%` : '--');
  value.font = Font.boldSystemFont(17);
  value.textColor = l ? colorFor(l.pct) : C.muted;
  stack.addSpacer(4);
  const bar = stack.addImage(barImage(width, 6, l ? l.raw : 0, l ? barColorFor(l.pct) : C.track));
  bar.imageSize = new Size(width, 6);
  if (showReset) {
    stack.addSpacer(3);
    const r = stack.addText(l ? `resets in ${l.reset}` : ' ');
    r.font = Font.systemFont(11);
    r.textColor = C.muted;
  }
}

function homeWidget(data, offline, family) {
  const w = new ListWidget();
  w.backgroundColor = C.bg;
  w.setPadding(14, 14, 12, 14);
  const width = family === 'small' ? 130 : 290;
  const five = limit(data, 'five_hour');
  const week = limit(data, 'seven_day');

  const head = w.addStack();
  const title = head.addText('Claude');
  title.font = Font.semiboldSystemFont(12);
  title.textColor = C.accent;
  head.addSpacer();
  const age = Math.round((Date.now() - (data.usageReceivedAt || 0)) / 1000);
  const st = head.addText(offline ? 'offline' : age > 600 ? `${duration(age)} ago` : 'live');
  st.font = Font.systemFont(10);
  st.textColor = C.muted;
  w.addSpacer(6);

  addLimitRow(w, family === 'small' ? '5h' : 'Session (5h)', five, width, true);
  w.addSpacer(8);
  addLimitRow(w, family === 'small' ? '7d' : 'Weekly (7d)', week, width, true);
  return w;
}

function lockWidget(data, family) {
  const w = new ListWidget();
  const five = limit(data, 'five_hour');
  const week = limit(data, 'seven_day');
  if (family === 'accessoryInline') {
    w.addText(`Claude 5h ${five ? five.pct + '%' : '--'} · 7d ${week ? week.pct + '%' : '--'}`);
  } else if (family === 'accessoryCircular') {
    const s = w.addStack();
    s.backgroundImage = ringImage(60, five ? five.raw : 0);
    s.size = new Size(60, 60);
    s.centerAlignContent();
    const t = s.addText(five ? `${five.pct}` : '--');
    t.font = Font.boldSystemFont(16);
    t.centerAlignText();
  } else {
    const t = w.addText('Claude');
    t.font = Font.semiboldSystemFont(13);
    for (const [label, l] of [['5h', five], ['7d', week]]) {
      const line = w.addText(`${label} ${l ? l.pct + '%' : '--'}  ·  ${l ? l.reset : '--'}`);
      line.font = Font.systemFont(13);
    }
  }
  return w;
}

function messageWidget(text) {
  const w = new ListWidget();
  w.backgroundColor = C.bg;
  const t = w.addText(text);
  t.font = Font.systemFont(12);
  t.textColor = C.muted;
  return w;
}

// Refresh again soon after the next reset so the widget flips promptly.
function nextRefresh(data) {
  const now = Date.now() / 1000;
  const resets = ['five_hour', 'seven_day']
    .map((k) => data.usage && data.usage[k] && data.usage[k].resetsAt)
    .filter((t) => t && t > now);
  const soon = Date.now() + 15 * 60_000;
  return new Date(Math.min(soon, ...resets.map((t) => (t + 60) * 1000)));
}

// ---- main ------------------------------------------------------------------------

let cfg = savedConfig();
if (!config.runsInWidget && args.queryParameters.reset) cfg = null;
let widget;
try {
  if (!cfg && !config.runsInWidget) cfg = await askConfig();
  if (!cfg) {
    widget = messageWidget('Run this script in Scriptable once to set it up.');
  } else {
    const { data, offline } = await loadUsage(cfg);
    const family = config.widgetFamily || 'medium';
    widget = family.startsWith('accessory') ? lockWidget(data, family) : homeWidget(data, offline, family);
    widget.refreshAfterDate = nextRefresh(data);
  }
} catch (err) {
  widget = messageWidget(`Claude usage unavailable: ${err.message}`);
}

if (config.runsInWidget) {
  Script.setWidget(widget);
} else {
  await widget.presentMedium();
}
Script.complete();
