// Hub (Durable Object) behaviour with an in-memory storage + alarm and a fake
// push service. Covers ingest auth, pairing, subscriptions, scheduling, alarms,
// de-duplication, preferences, widget keys and expired subscriptions.

import './setup.js';
import { describe, it, before, beforeEach, afterEach } from 'node:test';
import assert from 'node:assert/strict';
import nodeCrypto from 'node:crypto';
import { createRequire } from 'node:module';
import { Hub, isAllowedEndpoint } from '../src/hub.js';
import worker from '../src/index.js';
import { generateVapidKeys } from '../src/webpush.js';

const ece = createRequire(import.meta.url)('http_ece');
const PUSH_ORIGIN = 'https://push.test';
const WEEK = 7 * 86400;

function fakeState() {
  const map = new Map();
  const state = {
    alarm: null,
    storage: {
      get: async (k) => (map.has(k) ? structuredClone(map.get(k)) : undefined),
      put: async (k, v) => void map.set(k, structuredClone(v)),
      delete: async (k) => map.delete(k),
      setAlarm: async (t) => void (state.alarm = t),
      deleteAlarm: async () => void (state.alarm = null),
      getAlarm: async () => state.alarm,
    },
    map,
  };
  return state;
}

function subscription(pathName) {
  const ecdh = nodeCrypto.createECDH('prime256v1');
  ecdh.generateKeys();
  const auth = nodeCrypto.randomBytes(16);
  return {
    ecdh,
    auth,
    json: { endpoint: `${PUSH_ORIGIN}${pathName}`, keys: { p256dh: ecdh.getPublicKey().toString('base64url'), auth: auth.toString('base64url') } },
  };
}

const decrypt = (sub, body) =>
  JSON.parse(ece.decrypt(Buffer.from(body), { version: 'aes128gcm', privateKey: sub.ecdh, authSecret: sub.auth }).toString());

describe('Hub', () => {
  let env;
  let state;
  let hub;
  let sent; // requests received by the fake push service
  let realNow;

  const nowS = () => Math.floor(Date.now() / 1000);
  const call = (method, path, { body, token } = {}) =>
    hub.fetch(
      new Request(`https://cloud.test${path}`, {
        method,
        headers: { ...(token ? { Authorization: `Bearer ${token}` } : {}), 'Content-Type': 'application/json' },
        body: body === undefined ? undefined : typeof body === 'string' ? body : JSON.stringify(body),
      }),
    );
  const ingest = (path, body) => call(body === undefined ? 'GET' : 'POST', path, { body, token: env.INGEST_TOKEN });
  const usage = (fiveReset, weekReset, five = 50, week = 20) => ({
    fetched_at: nowS(),
    plan: 'pro',
    five_hour: fiveReset === null ? null : { utilization_x100: five * 100, resets_at: fiveReset },
    seven_day: { utilization_x100: week * 100, resets_at: weekReset },
    seven_day_sonnet: null,
    seven_day_opus: null,
  });

  async function pairDevice() {
    const { code } = await (await ingest('/ingest/pair', {})).json();
    const res = await call('POST', '/api/pair', { body: { code } });
    assert.equal(res.status, 201);
    return (await res.json()).token;
  }

  async function pairAndSubscribe(pathName = '/phone') {
    const token = await pairDevice();
    const sub = subscription(pathName);
    assert.equal((await call('POST', '/api/subscribe', { token, body: { subscription: sub.json } })).status, 200);
    return { token, sub };
  }

  before(async () => {
    const keys = await generateVapidKeys();
    env = {
      VAPID_PUBLIC_KEY: keys.publicKey,
      VAPID_PRIVATE_KEY: keys.privateKey,
      INGEST_TOKEN: nodeCrypto.randomBytes(32).toString('base64url'),
      TEST_PUSH_ORIGIN: PUSH_ORIGIN,
    };
  });

  beforeEach(() => {
    realNow = Date.now;
    state = fakeState();
    hub = new Hub(state, env);
    sent = [];
    hub.fetchImpl = async (url, init) => {
      sent.push({ url, headers: init.headers, body: init.body });
      return new Response(null, { status: url.includes('/gone') ? 410 : 201 });
    };
  });

  afterEach(() => {
    Date.now = realNow;
  });

  const advance = (seconds) => {
    const base = Date.now();
    Date.now = () => base + seconds * 1000;
  };

  it('requires the ingest token', async () => {
    assert.equal((await call('GET', '/ingest/health')).status, 401);
    assert.equal((await call('GET', '/ingest/health', { token: 'x'.repeat(43) })).status, 401);
    assert.equal((await ingest('/ingest/health')).status, 200);
  });

  it('pairs once, locks after wrong guesses, and stores only token hashes', async () => {
    assert.equal((await call('POST', '/api/pair', { body: { code: 'ABCD-EFGH' } })).status, 403);
    const { code } = await (await ingest('/ingest/pair', {})).json();
    for (let i = 0; i < 5; i++) assert.equal((await call('POST', '/api/pair', { body: { code: 'ZZZZ-ZZZZ' } })).status, 403);
    assert.equal((await call('POST', '/api/pair', { body: { code } })).status, 403, 'locked');

    const token = await pairDevice();
    assert.ok(!JSON.stringify([...state.map.values()]).includes(token));
    assert.equal((await call('GET', '/api/status', { token })).status, 200);
    assert.equal((await call('GET', '/api/status')).status, 401);
  });

  it('only accepts subscriptions on real push services', async () => {
    const token = await pairDevice();
    const evil = { ...subscription('/x').json, endpoint: 'https://evil.example.com/x' };
    assert.equal((await call('POST', '/api/subscribe', { token, body: { subscription: evil } })).status, 400);
    const apple = { ...subscription('/x').json, endpoint: 'https://web.push.apple.com/QwErTy' };
    assert.equal((await call('POST', '/api/subscribe', { token, body: { subscription: apple } })).status, 200);
  });

  it('stores usage and schedules the earliest future reset', async () => {
    const five = nowS() + 3600;
    const week = nowS() + 86400;
    assert.equal((await ingest('/ingest/usage', usage(five, week))).status, 204);
    assert.equal(state.alarm, (five - 1800) * 1000, 'first: the 30-minute reminder (the 1-hour one would be now)');
    const health = await (await ingest('/ingest/health')).json();
    assert.deepEqual(health.schedule, { five_hour_reset: five, weekly_reset: week });
    assert.equal((await ingest('/ingest/usage', { ...usage(five, week), five_hour: { utilization_x100: 'x' } })).status, 400);
  });

  it('does not schedule resets that are already in the past', async () => {
    await ingest('/ingest/usage', usage(nowS() - 10, nowS() - 5));
    assert.equal(state.alarm, null);
  });

  it('fires the 5-hour alert at the reset time with the laptop offline', async () => {
    const { sub } = await pairAndSubscribe();
    const five = nowS() + 60;
    const week = nowS() + 3 * 86400;
    await ingest('/ingest/usage', usage(five, week, 10, 10));

    advance(61); // laptop is now offline; only the alarm runs
    await hub.alarm();
    assert.equal(sent.length, 1);
    const note = decrypt(sub, sent[0].body);
    assert.equal(note.title, 'Claude: 5h limit reset');
    assert.equal(note.body, 'Full usage available.');
    const health = await (await ingest('/ingest/health')).json();
    assert.equal(health.schedule.five_hour_reset, 0, 'next 5h window is unknown until the laptop reports');
    assert.equal(state.alarm, (week - 3600) * 1000);
  });

  it('fires the weekly alert and rolls the next one forward a week', async () => {
    const { sub } = await pairAndSubscribe();
    const week = nowS() + 30;
    await ingest('/ingest/usage', usage(0, week));
    advance(31);
    await hub.alarm();
    assert.equal(decrypt(sub, sent[0].body).title, 'Claude: weekly limit reset');
    assert.equal(state.alarm, (week + WEEK - 3600) * 1000);

    advance(31 + WEEK); // still offline a week later: fires again
    await hub.alarm();
    assert.equal(sent.length, 2);
    assert.equal(decrypt(sub, sent[1].body).title, 'Claude: weekly limit reset');
  });

  it('never double-notifies when the laptop also reports the reset', async () => {
    await pairAndSubscribe();
    const five = nowS() + 10;
    await ingest('/ingest/usage', usage(five, nowS() + 86400, 10, 10));
    advance(11);
    await hub.alarm();
    assert.equal(sent.length, 1);
    const res = await ingest('/ingest/event', { id: `five_hour_reset:${five}`, kind: 'five_hour_reset', reset_at: five });
    assert.equal((await res.json()).duplicate, true);
    await hub.alarm(); // a repeated alarm doesn't resend either
    assert.equal(sent.length, 1);
  });

  it('accepts confirmed events from the laptop for resets it scheduled late', async () => {
    const { sub } = await pairAndSubscribe();
    const t = nowS() - 20;
    const res = await ingest('/ingest/event', { id: `weekly_reset:${t}`, kind: 'weekly_reset', reset_at: t });
    assert.equal(res.status, 202);
    assert.equal(decrypt(sub, sent[0].body).title, 'Claude: weekly limit reset');
    assert.equal((await ingest('/ingest/event', { id: 'weekly_reset:1', kind: 'weekly_reset', reset_at: t })).status, 400);
  });

  it('respects per-kind preferences', async () => {
    const { token } = await pairAndSubscribe();
    assert.equal((await call('PUT', '/api/prefs', { token, body: { weekly_reset: false } })).status, 200);
    assert.equal((await call('PUT', '/api/prefs', { token, body: { weekly_reset: 'no' } })).status, 400);
    const t = nowS() - 1;
    await ingest('/ingest/event', { id: `weekly_reset:${t}`, kind: 'weekly_reset', reset_at: t });
    assert.equal(sent.length, 0);
  });

  it('removes subscriptions the push service reports gone', async () => {
    const { token } = await pairAndSubscribe('/gone');
    const t = nowS() - 1;
    await ingest('/ingest/event', { id: `five_hour_reset:${t}`, kind: 'five_hour_reset', reset_at: t });
    const status = await (await call('GET', '/api/status', { token })).json();
    assert.equal(status.device.subscribed, false);
  });

  it('sends rate-limited test notifications 10 seconds later', async () => {
    const { token, sub } = await pairAndSubscribe();
    const { token: other } = await pairAndSubscribe('/other-phone');
    const res = await call('POST', '/api/test', { token });
    assert.equal(res.status, 200);
    assert.equal(sent.length, 0, 'not yet: time to lock the phone');
    assert.equal(state.alarm, (nowS() + 10) * 1000);
    assert.equal((await call('POST', '/api/test', { token })).status, 429);
    advance(10);
    await hub.alarm();
    assert.equal(sent.length, 1, 'only to the phone that asked');
    const note = decrypt(sub, sent[0].body);
    assert.equal(note.title, 'Claude Usage test');
    assert.equal(note.body, 'Notifications are working.', 'nothing uploaded yet');
    const health = await (await ingest('/ingest/health')).json();
    assert.equal(health.log.at(-1).title, 'Claude Usage test');
    assert.equal(health.log.at(-1).to, 'you');
    await hub.alarm();
    assert.equal(sent.length, 1, 'once');
    assert.ok(other);
  });

  it('issues read-only widget keys', async () => {
    const token = await pairDevice();
    await ingest('/ingest/usage', usage(nowS() + 3600, nowS() + 86400, 64, 22));
    const { key } = await (await call('POST', '/api/widget-key', { token })).json();
    const widget = await call('GET', '/api/widget', { token: key });
    assert.equal(widget.status, 200);
    const body = await widget.json();
    assert.equal(body.usage.five_hour.percent, 64);
    assert.ok(Array.isArray(body.accounts), 'the rings widgets get every account');
    assert.equal((await call('GET', '/api/status', { token: key })).status, 401, 'widget key cannot manage the device');
    assert.equal((await call('POST', '/api/test', { token: key })).status, 401);
    const { key: rotated } = await (await call('POST', '/api/widget-key', { token })).json();
    assert.equal((await call('GET', '/api/widget', { token: key })).status, 401, 'old key revoked');
    assert.equal((await call('GET', '/api/widget', { token: rotated })).status, 200);
  });

  it('sends account-switch notices once, honouring the preference', async () => {
    const { token, sub } = await pairAndSubscribe();
    const notice = { id: 'switch:1', title: 'Claude account switched', body: 'nayef used up its 5-hour limit (96%). Now using huey.' };
    assert.equal((await ingest('/ingest/notice', notice)).status, 202);
    assert.deepEqual(decrypt(sub, sent[0].body), { ...decrypt(sub, sent[0].body), title: notice.title, body: notice.body });
    assert.equal((await (await ingest('/ingest/notice', notice)).json()).duplicate, true);
    assert.equal(sent.length, 1);
    assert.equal((await ingest('/ingest/notice', { ...notice, id: 'bad id!' })).status, 400);
    assert.equal((await ingest('/ingest/notice', { ...notice, id: 'x', title: 'y'.repeat(81) })).status, 400);
    await call('PUT', '/api/prefs', { token, body: { account_switch: false } });
    await ingest('/ingest/notice', { ...notice, id: 'switch:2' });
    assert.equal(sent.length, 1, 'muted by preference');
  });

  describe('account owners', () => {
    const acct = (email, name, active, five, week, fiveReset, weekReset) => ({
      email, name, active,
      five_hour: { utilization_x100: five * 100, resets_at: fiveReset },
      seven_day: { utilization_x100: week * 100, resets_at: weekReset },
    });
    const upload = (accounts) => ingest('/ingest/accounts', { user_name: 'Nayef', accounts });

    async function pairOwner(name) {
      const res = await ingest('/ingest/pair', { owner: name });
      assert.equal(res.status, 200);
      const { code, owner } = await res.json();
      assert.equal(owner, name);
      const paired = await call('POST', '/api/pair', { body: { code } });
      const { token, role } = await paired.json();
      assert.equal(role, 'owner');
      const sub = subscription(`/owner-${name}`);
      assert.equal((await call('POST', '/api/subscribe', { token, body: { subscription: sub.json } })).status, 200);
      return { token, sub };
    }
    const to = (pathName) => sent.filter((s) => s.url.endsWith(pathName));

    it('keeps an automatic 5-hour cap at the suggested one, quietly, until it is set by hand', async () => {
      const t = nowS();
      // Learnt costs like huey's: a full 5-hour session is about 15% of his week.
      const huey = (week) => [{ ...acct('hueyduck@example.com', 'huey', false, 3, week, t + 3 * 3600, t + 5 * 86400),
        prompt_cost: { five_hour: { low: 41, medium: 51, high: 145 }, seven_day: { low: 6, medium: 8, high: 22 } } }];
      const { token: me } = await pairAndSubscribe('/me');
      await upload(huey(28));
      await pairOwner('huey');
      const log = async () => (await (await ingest('/ingest/health')).json()).log.map((e) => `${e.to}: ${e.title}`);
      await call('PUT', '/api/my-cap', { token: me, body: { account: 'huey', cap_five_hour: 70, cap_weekly: null } });
      const before = (await log()).length;
      // On: 72% of his week left = 475% of 5-hour sessions, plus the 3% this one has used, over this
      // session and the 23.4 after it before the weekly reset = 19.6% of each, as 15%.
      let caps = (await (await call('PUT', '/api/auto-cap', { token: me, body: { account: 'huey', on: true } })).json()).caps;
      assert.deepEqual([caps.auto_five_hour, caps.mine_five_hour], [true, 15]);
      // huey is told once that it's on.
      const autoNotes = async () => (await log()).filter((e) => e.includes('Auto-cap'));
      assert.deepEqual(await autoNotes(), ['huey: Nayef turned on Auto-cap']);
      // It follows usage, and nobody is told.
      const rule = (await (await upload(huey(48))).json()).policies[0];
      assert.equal(rule.cap_five_hour, 10);  // 52% left: 14.2%
      assert.equal(rule.auto_five_hour, true);
      assert.equal((await log()).length, before + 1, 'automatic changes notify no one');
      // Off: back to the 70% set before, and huey is told once.
      caps = (await (await call('PUT', '/api/auto-cap', { token: me, body: { account: 'huey', on: false } })).json()).caps;
      assert.deepEqual([caps.auto_five_hour, caps.mine_five_hour], [false, 70]);
      assert.equal((await autoNotes()).filter((e) => e === 'huey: Nayef turned off Auto-cap').length, 1);
      // On again, then a cap set by hand: that one wins and auto is off.
      await call('PUT', '/api/auto-cap', { token: me, body: { account: 'huey', on: true } });
      await call('PUT', '/api/my-cap', { token: me, body: { account: 'huey', cap_five_hour: 40, cap_weekly: null } });
      const after = (await (await call('GET', '/api/status', { token: me })).json()).accounts[0].caps;
      assert.deepEqual([after.auto_five_hour, after.mine_five_hour], [false, 40]);
      assert.equal((await call('PUT', '/api/auto-cap', { token: (await pairOwner('huey')).token, body: { account: 'huey', on: true } })).status, 403);
    });

    it("lets an owner put their own 5-hour cap on auto, seeing only their account's suggestion", async () => {
      const t = nowS();
      const cost = { five_hour: { low: 41, medium: 51, high: 145 }, seven_day: { low: 6, medium: 8, high: 22 } };
      const both = (hueyWeek) => [
        { ...acct('hueyduck@example.com', 'huey', false, 3, hueyWeek, t + 3 * 3600, t + 5 * 86400), prompt_cost: cost },
        { ...acct('nayeftest7@example.com', 'nayef', true, 3, 24, t + 3 * 3600, t + 5 * 86400), prompt_cost: cost },
      ];
      await upload(both(54));
      const { token: huey } = await pairOwner('huey');
      const owner = (await pairAndSubscribe('/owner')).token;
      await call('PUT', '/api/owner/policy', { token: huey, body: { cap_five_hour: 70, cap_weekly: null, allow: true } });
      const log = async () => (await (await ingest('/ingest/health')).json()).log.length;
      // Their status: their suggestion (the same as the user's), not the other accounts.
      const view = await (await call('GET', '/api/status', { token: huey })).json();
      assert.deepEqual(view.suggested, { five_hour: 10, auto: false });  // his own: 46% left, 12.5% a session
      assert.equal(JSON.stringify(view).includes('nayef'), false);
      assert.equal((await call('PUT', '/api/owner/auto-cap', { token: owner, body: { on: true } })).status, 403);
      const before = await log();
      const on = await (await call('PUT', '/api/owner/auto-cap', { token: huey, body: { on: true } })).json();
      assert.deepEqual([on.policy.cap_five_hour, on.suggested.auto], [10, true]);
      assert.equal(await log(), before + 1, 'the user is told once that it is on');
      // It follows usage, quietly, and the PC is told it's automatic.
      const rule = (await (await upload(both(64))).json()).policies.find((p) => p.email === 'hueyduck@example.com');
      assert.deepEqual([rule.owner_cap_five_hour, rule.owner_auto_five_hour], [5, true]);
      assert.equal(await log(), before + 1, 'automatic changes notify no one');
      // Off: back to their 70%.
      const off = await (await call('PUT', '/api/owner/auto-cap', { token: huey, body: { on: false } })).json();
      assert.deepEqual([off.policy.cap_five_hour, off.suggested.auto], [70, false]);
    });

    it("works each account's suggested 5-hour cap out on its own", async () => {
      const t = nowS();
      const cost = { five_hour: { low: 41, medium: 51, high: 145 }, seven_day: { low: 6, medium: 8, high: 22 } };
      const { token: me } = await pairAndSubscribe('/me');
      // Both: this 5-hour session (3% used) and 23.4 more before the weekly reset in 5 days.
      // huey: 46% of his week left = 12.5% a session, as 10%; nayef: 76% left = 20.7%, as 20%.
      await upload([
        { ...acct('hueyduck@example.com', 'huey', false, 3, 54, t + 3 * 3600, t + 5 * 86400), prompt_cost: cost },
        { ...acct('nayeftest7@example.com', 'nayef', true, 3, 24, t + 3 * 3600, t + 5 * 86400), prompt_cost: cost },
      ]);
      const on = async (account) => (await (await call('PUT', '/api/auto-cap', { token: me, body: { account, on: true } })).json()).caps.mine_five_hour;
      assert.deepEqual([await on('huey'), await on('nayef')], [10, 20]);
    });

    it("passes the PC's last and next usage check on to every phone (the countdown)", async () => {
      const t = nowS();
      const { token: me } = await pairAndSubscribe('/me');
      await ingest('/ingest/accounts', { user_name: 'Nayef', accounts: [acct('hueyduck@example.com', 'huey', true, 3, 20, t + 3600, t + 86400)],
        usage_check: { at: t - 30, next: t + 110 } });
      const { token: huey } = await pairOwner('huey');
      for (const token of [me, huey]) {
        assert.deepEqual((await (await call('GET', '/api/status', { token })).json()).usageCheck, { at: t - 30, next: t + 110 });
      }
    });

    it('gives an account whose week resets sooner a higher cap (fewer sessions left)', async () => {
      const t = nowS();
      const cost = { five_hour: { low: 41, medium: 51, high: 145 }, seven_day: { low: 6, medium: 8, high: 22 } };
      const { token: me } = await pairAndSubscribe('/me');
      // huey's week resets in a day: this session and 4.2 more, so 306% over 5.2 = 59%, as 55%.
      // nayef's in 5 days: 20%, as before.
      await upload([
        { ...acct('hueyduck@example.com', 'huey', false, 3, 54, t + 3 * 3600, t + 86400), prompt_cost: cost },
        { ...acct('nayeftest7@example.com', 'nayef', true, 3, 24, t + 3 * 3600, t + 5 * 86400), prompt_cost: cost },
      ]);
      const on = async (account) => (await (await call('PUT', '/api/auto-cap', { token: me, body: { account, on: true } })).json()).caps.mine_five_hour;
      assert.deepEqual([await on('huey'), await on('nayef')], [55, 20]);
    });

    it('lets a friend free the 5-hour reserve their cap keeps, until the reset', async () => {
      const t = nowS();
      const huey = (five) => [acct('hueyduck@example.com', 'huey', false, five, 10, t + 25 * 60, t + 86400)];
      const { token: me } = await pairAndSubscribe('/me');
      await upload(huey(30));
      const { token: owner } = await pairOwner('huey');
      await call('PUT', '/api/owner/policy', { token: owner, body: { cap_five_hour: 60, cap_weekly: null, allow: true } });
      const log = async () => (await (await ingest('/ingest/health')).json()).log.map((e) => `${e.to}: ${e.title} | ${e.body}`);
      // 25 minutes before his reset, with 40% held back: he is asked, once.
      await upload(huey(30));
      await hub.alarm();
      await upload(huey(31));
      await hub.alarm();
      const offers = (await log()).filter((l) => l.includes('Let Nayef use your'));
      assert.equal(offers.length, 1, offers.join('\n'));
      assert.ok(offers[0].startsWith('huey: Your 5-hour limit resets in') && offers[0].includes('your 40%'), offers[0]);
      let s = await (await call('GET', '/api/status', { token: owner })).json();
      assert.deepEqual([s.reserve.cap, s.reserve.free, s.reserve.released], [60, 40, false]);
      // He frees it: the user hears, the cap is lifted, and the bar's green starts at 60%.
      assert.equal((await (await call('POST', '/api/reserve', { token: owner, body: { release: true } })).json()).reserve.released, true);
      assert.ok((await log()).some((l) => l.startsWith('you: huey: the rest of the 5 hours is yours')));
      let rule = (await (await upload(huey(31))).json()).policies[0];
      assert.equal(rule.cap_five_hour, null);
      assert.equal(rule.reserve_from, 60);
      assert.equal((await (await call('GET', '/api/status', { token: me })).json()).accounts[0].caps.reserve.from, 60);
      // Taken back: the cap holds again. Only the owner can do this.
      await call('POST', '/api/reserve', { token: owner, body: { release: false } });
      rule = (await (await upload(huey(31))).json()).policies[0];
      assert.equal(rule.cap_five_hour, 60);
      assert.equal(rule.reserve_from, null);
      assert.equal((await call('POST', '/api/reserve', { token: me, body: { release: true } })).status, 403);
      // The user can ask for it (once every few minutes), with the reset time in the notification.
      const ask = await call('POST', '/api/ask-reserve', { token: me, body: { account: 'huey', clock: '3:45 PM' } });
      assert.equal(ask.status, 200);
      assert.ok((await log()).some((l) => /^huey: Nayef is requesting your remaining usage \| \d+m left before reset \(3:45 PM\)$/.test(l)), (await log()).join('\n'));
      assert.equal((await call('POST', '/api/ask-reserve', { token: me, body: { account: 'huey' } })).status, 429);
      assert.equal((await call('POST', '/api/ask-reserve', { token: owner, body: { account: 'huey' } })).status, 403);
      // It ends by itself at the reset.
      await call('POST', '/api/reserve', { token: owner, body: { release: true } });
      advance(26 * 60);
      rule = (await (await upload([acct('hueyduck@example.com', 'huey', false, 2, 10, t + 26 * 60 + 5 * 3600, t + 86400)])).json()).policies[0];
      assert.equal(rule.cap_five_hour, 60, 'the new window has the cap again');
      assert.equal(rule.reserve_from, null);
    });

    it('pairs an owner for one account and shows them only that account', async () => {
      const t = nowS();
      await upload([acct('hueyduck@example.com', 'huey', true, 20, 10, t + 3600, t + 86400),
                    acct('nayeftest7@example.com', 'nayef', false, 90, 35, t + 7200, t + 90000)]);
      assert.equal((await ingest('/ingest/pair', { owner: 'spud' })).status, 404);
      const { token } = await pairOwner('huey');
      const status = await (await call('GET', '/api/status', { token })).json();
      assert.equal(status.role, 'owner');
      assert.equal(status.account.name, 'huey');
      assert.equal(status.account.five_hour.percent, 20);
      assert.equal(status.userName, 'Nayef');
      assert.ok(!JSON.stringify(status).includes('nayeftest7'), 'other accounts are never exposed');
      assert.equal(status.usage, undefined);
      const widgetKey = (await (await call('POST', '/api/widget-key', { token })).json()).key;
      const widget = await (await call('GET', '/api/widget', { token: widgetKey })).json();
      assert.equal(widget.usage.name, 'huey');
      // Nothing a friend's phone can read mentions another account: page, widget, live tokens.
      const live = await (await call('GET', '/api/tokens', { token })).json();
      for (const [what, body] of [['status', status], ['widget', widget], ['tokens', live]]) {
        const raw = JSON.stringify(body);
        assert.ok(!raw.includes('nayef') && !raw.includes('nayeftest7'), `${what} shows only huey's account`);
      }
      assert.deepEqual(widget.accounts.map((x) => x.name), ['huey']);
    });

    it('notifies the owner on start/stop, 50%, maxed out and reset - each once', async () => {
      const t = nowS();
      const five = t + 3600;
      const week = t + 86400;
      await upload([acct('hueyduck@example.com', 'huey', false, 10, 5, five, week)]);
      const { sub } = await pairOwner('huey');
      const owner = () => to('/owner-huey').map((s) => decrypt(sub, s.body));

      await upload([acct('hueyduck@example.com', 'huey', true, 12, 5, five, week)]);
      assert.equal(owner().at(-1).title, 'Nayef is now on your account');

      await upload([acct('hueyduck@example.com', 'huey', true, 52, 6, five, week)]);
      let last = owner().at(-1);
      assert.equal(last.title, 'huey: 52% of 5h used');
      assert.match(last.body, /^48% left · resets in (59m|1h) · Nayef on it$/);
      const count = owner().length;
      await upload([acct('hueyduck@example.com', 'huey', true, 55, 6, five, week)]);
      assert.equal(owner().length, count, 'no repeat within the same window');

      await upload([acct('hueyduck@example.com', 'huey', true, 100, 7, five, week)]);
      last = owner().at(-1);
      assert.equal(last.title, 'huey: 5h limit reached');
      assert.equal(owner().length, count + 1, 'jumping past 75 and 85 sends one notification, not three');

      await upload([acct('hueyduck@example.com', 'huey', false, 100, 7, five, week)]);
      assert.equal(owner().at(-1).title, 'Nayef left your account');
      assert.equal(owner().at(-1).body, '0% of your 5h left for you (weekly 93%). Nayef is inactive now.');

      advance(3601);
      await hub.alarm();
      assert.equal(owner().at(-1).title, 'huey: 5h limit reset');
      assert.equal(state.alarm, (week - 3600) * 1000, 'next alarm: an hour before the weekly reset');
    });

    it("lets the owner cap usage and ask the user to stop; the laptop gets the rules back", async () => {
      const t = nowS();
      const accounts = [acct('hueyduck@example.com', 'huey', true, 40, 10, t + 3600, t + 86400)];
      await upload(accounts);
      const { token: ownerToken, sub: ownerSub } = await pairOwner('huey');
      const { sub: userSub } = await pairAndSubscribe('/nayef-phone');
      const user = () => to('/nayef-phone').map((s) => decrypt(userSub, s.body));

      assert.equal((await call('PUT', '/api/owner/policy', { token: ownerToken, body: { cap_five_hour: 3, cap_weekly: null, allow: true } })).status, 400);
      assert.equal((await call('PUT', '/api/owner/policy', { token: ownerToken, body: { cap_five_hour: 70, cap_weekly: null, allow: true } })).status, 200);
      assert.equal(user().at(-1).body, 'Capped at 5h 70%.');

      let res = await (await upload(accounts)).json();
      assert.deepEqual(res.policies, [{
        email: 'hueyduck@example.com', cap_five_hour: 70, cap_weekly: null, allow: true,
        owner_cap_five_hour: 70, owner_cap_weekly: null, updated_at: res.policies[0].updated_at, reserve_from: null, auto_five_hour: false, owner_auto_five_hour: false, no_auto: false,
      }]);

      await upload([acct('hueyduck@example.com', 'huey', true, 71, 10, t + 3600, t + 86400)]);
      const ownerNotes = to('/owner-huey').map((s) => decrypt(ownerSub, s.body));
      assert.equal(ownerNotes.at(-1).title, 'huey: 70% 5h cap hit');

      await call('PUT', '/api/owner/policy', { token: ownerToken, body: { cap_five_hour: 70, cap_weekly: null, allow: false } });
      assert.equal(user().at(-1).body, 'Asked you to stop using it.');
      res = await (await upload(accounts)).json();
      assert.equal(res.policies[0].allow, false);

      // The user's own controls stay the user's.
      const { token: userToken } = await pairAndSubscribe('/nayef-2');
      assert.equal((await call('PUT', '/api/owner/policy', { token: userToken, body: { cap_five_hour: null, cap_weekly: null, allow: true } })).status, 403);
    });

    it('keeps several pairing codes pending at once, each for a day', async () => {
      const t = nowS();
      await upload([acct('hueyduck@example.com', 'huey', true, 10, 5, t + 3600, t + 86400),
                    acct('nayeftest7@example.com', 'nayef', false, 10, 5, t + 3600, t + 86400)]);
      const mine = await (await ingest('/ingest/pair', {})).json();
      const huey = await (await ingest('/ingest/pair', { owner: 'HUEY' })).json();
      const nayef = await (await ingest('/ingest/pair', { owner: 'nayef' })).json();
      assert.ok(huey.expiresAt - Date.now() > 23 * 3600_000);
      advance(23 * 3600);
      const res = await call('POST', '/api/pair', { body: { code: huey.code } });
      assert.equal((await res.json()).role, 'owner');
      for (let i = 0; i < 6; i++) {  // reopening a used link: refused, but never voids the other links
        const again = await call('POST', '/api/pair', { body: { code: huey.code } });
        assert.equal(again.status, 410);
        assert.match((await again.json()).error, /already used/);
      }
      assert.equal((await (await call('POST', '/api/pair', { body: { code: mine.code } })).json()).role, 'user');
      advance(2 * 3600);  // a day later the unused code is gone
      assert.equal((await call('POST', '/api/pair', { body: { code: nayef.code } })).status, 403);
    });

    it('shows the user every account with prompt estimates, and each owner only theirs', async () => {
      const t = nowS();
      const cost = { five_hour: { low: 150, medium: 300, high: 700 }, seven_day: null };
      await upload([
        { ...acct('nayeftest7@example.com', 'nayef', true, 40, 20, t + 3600, t + 86400), prompt_cost: cost },
        acct('hueyduck@example.com', 'huey', false, 10, 5, t + 7200, t + 90000),
      ]);
      const { token: ownerToken } = await pairOwner('huey');
      const { token: userToken } = await pairAndSubscribe('/nayef-phone');

      const mine = await (await call('GET', '/api/status', { token: userToken })).json();
      assert.deepEqual(mine.accounts.map((a) => [a.name, a.active]), [['nayef', true], ['huey', false]]);
      assert.deepEqual(mine.accounts[0].prompt_cost, { five_hour: { low: 1.5, medium: 3, high: 7 }, seven_day: null });
      assert.equal(mine.accounts[1].prompt_cost, null);
      assert.ok(!JSON.stringify(mine).includes('@'), 'no emails reach a phone');

      const hueys = await (await call('GET', '/api/status', { token: ownerToken })).json();
      assert.equal(hueys.accounts, undefined);
      assert.equal(hueys.account.name, 'huey');

      const bad = await upload([{ ...acct('a@b.c', 'x', true, 1, 1, t + 60, t + 600), prompt_cost: { five_hour: { low: 0, medium: 1, high: 2 } } }]);
      assert.equal(bad.status, 400);
    });

    it('tells the user when any account resets, and each owner about their own', async () => {
      const t = nowS();
      await upload([acct('nayeftest7@example.com', 'nayef', true, 40, 20, t + 3600, t + 86400),
                    acct('hueyduck@example.com', 'huey', false, 90, 30, t + 1800, t + 90000)]);
      const { sub: ownerSub } = await pairOwner('huey');
      const { sub: userSub } = await pairAndSubscribe('/nayef-phone');
      const all = () => to('/nayef-phone').map((s) => decrypt(userSub, s.body).title);
      const user = () => all().filter((x) => / limit reset$/.test(x));

      // The live account's own reset path stays quiet: the per-account alert covers it.
      await ingest('/ingest/event', { id: `five_hour_reset:${t - 5}`, kind: 'five_hour_reset', reset_at: t - 5 });
      assert.deepEqual(user(), []);

      advance(1801);
      await hub.alarm();
      assert.deepEqual(user(), ['huey: 5h limit reset']);
      assert.ok(all().includes('nayef: 5h resets in 29m'), "and the reminder for nayef's reset, due now");
      assert.equal(decrypt(ownerSub, to('/owner-huey').at(-1).body).title, 'huey: 5h limit reset');

      advance(1800);
      await hub.alarm();
      assert.deepEqual(user(), ['huey: 5h limit reset', 'nayef: 5h limit reset']);
      assert.equal(to('/owner-huey').length, 1, "huey never hears about nayef's account");
      await hub.alarm();
      assert.equal(user().length, 2, 'each reset once');
    });

    it('sends usage alerts at 25/50/75/85/100%, each one switchable per phone', async () => {
      const t = nowS();
      const five = t + 3600;
      const week = t + 86400;
      await upload([acct('hueyduck@example.com', 'huey', true, 10, 5, five, week)]);
      const { token: meToken, sub: meSub } = await pairAndSubscribe('/me');
      const { token: ownerToken, sub: ownerSub } = await pairOwner('huey');
      const me = () => to('/me').map((s) => decrypt(meSub, s.body).title);
      const huey = () => to('/owner-huey').map((s) => decrypt(ownerSub, s.body).title).filter((x) => /used|cap|reached/.test(x));
      assert.equal((await call('PUT', '/api/prefs', { token: meToken, body: { usage_50: false } })).status, 200);
      assert.equal((await call('PUT', '/api/prefs', { token: ownerToken, body: { usage_25: false } })).status, 200);

      await upload([acct('hueyduck@example.com', 'huey', true, 26, 5, five, week)]);
      assert.deepEqual(me(), ['huey: 26% of 5h used']);
      assert.deepEqual(huey(), [], '25% is off on huey\'s phone');
      await upload([acct('hueyduck@example.com', 'huey', true, 52, 5, five, week)]);
      assert.equal(me().length, 1, '50% is off on mine');
      assert.deepEqual(huey(), ['huey: 52% of 5h used']);
      await upload([acct('hueyduck@example.com', 'huey', true, 86, 5, five, week)]);
      assert.deepEqual(me().slice(1), ['huey: 86% of 5h used'], 'one alert for 75 and 85 together');
      await upload([acct('hueyduck@example.com', 'huey', true, 100, 5, five, week)]);
      assert.equal(me().at(-1), 'huey: 5h limit reached');
      assert.equal(huey().at(-1), 'huey: 5h limit reached');
    });

    it('never repeats a weekly alert, however many other notifications go out that week', async () => {
      const t = nowS();
      const accounts = [acct('hueyduck@example.com', 'huey', true, 10, 55, t + 3600, t + 6 * 86400)];
      const { sub } = await pairAndSubscribe('/me');
      await upload(accounts);
      const weekly = () => to('/me').map((s) => decrypt(sub, s.body).title).filter((x) => /of weekly used/.test(x));
      assert.deepEqual(weekly(), ['huey: 55% of weekly used']);
      for (let i = 0; i < 1200; i++) await ingest('/ingest/notice', { id: `n${i}`, title: 'x', body: 'x' });
      await upload(accounts);
      assert.equal(weekly().length, 1);
    });

    it("puts the live numbers of each phone's own accounts in its test notification", async () => {
      const t = nowS();
      await upload([acct('nayeftest7@example.com', 'nayef', true, 95.4, 52, t + 3600, t + 86400),
                    acct('hueyduck@example.com', 'huey', false, 16, 21, t + 7200, t + 90000)]);
      const { token, sub } = await pairAndSubscribe('/me');
      const { token: hueyToken, sub: hueySub } = await pairOwner('huey');
      await call('POST', '/api/test', { token });
      await call('POST', '/api/test', { token: hueyToken });
      advance(10);
      await hub.alarm();
      assert.equal(decrypt(sub, to('/me')[0].body).body, 'Now: nayef 5h 95% · wk 52%  |  huey 5h 16% · wk 21%');
      assert.equal(decrypt(hueySub, to('/owner-huey')[0].body).body, 'Now: huey 5h 16% · wk 21%');
    });

    it('reminds 1 h, 30, 15 and 5 minutes before a reset, each switchable', async () => {
      const t = nowS();
      const five = t + 2 * 3600;
      await upload([acct('hueyduck@example.com', 'huey', true, 60, 5, five, t + 5 * 86400)]);
      const { token, sub } = await pairAndSubscribe('/me');
      const { sub: ownerSub } = await pairOwner('huey');
      await call('PUT', '/api/prefs', { token, body: { remind_30: false } });
      const me = () => to('/me').map((s) => decrypt(sub, s.body).title).filter((x) => /resets in/.test(x));
      const owner = () => to('/owner-huey').map((s) => decrypt(ownerSub, s.body).title).filter((x) => /resets in/.test(x));
      for (const m of [60, 30, 15, 5]) {
        assert.equal(state.alarm, (five - m * 60) * 1000, `alarm for the ${m}-minute reminder`);
        advance(state.alarm / 1000 - nowS());
        await hub.alarm();
      }
      assert.deepEqual(me(), ['huey: 5h resets in 1h', 'huey: 5h resets in 15m', 'huey: 5h resets in 5m']);
      assert.deepEqual(owner(), ['huey: 5h resets in 1h', 'huey: 5h resets in 30m', 'huey: 5h resets in 15m', 'huey: 5h resets in 5m']);
      assert.equal(state.alarm, five * 1000, 'then the reset itself');
    });

    it("shows what's new on an account's usage data (limit resets, someday) only while it's there", async () => {
      const t = nowS();
      const { token: me } = await pairAndSubscribe('/me');
      const accounts = [acct('hueyduck@example.com', 'huey', true, 3, 20, t + 3600, t + 86400)];
      await ingest('/ingest/accounts', { user_name: 'Nayef', accounts, extras: { email: 'hueyduck@example.com', items: [{ key: 'harbor_lantern', ends_at: t + 9 * 86400 }] } });
      const shown = async () => (await (await call('GET', '/api/status', { token: me })).json()).accounts[0].extras;
      assert.deepEqual(await shown(), [{ key: 'harbor_lantern', endsAt: t + 9 * 86400 }]);
      const { token: huey } = await pairOwner('huey');
      assert.equal((await (await call('GET', '/api/status', { token: huey })).json()).extras.length, 1);
      await ingest('/ingest/accounts', { user_name: 'Nayef', accounts, extras: { email: 'hueyduck@example.com', items: [] } });
      assert.deepEqual(await shown(), []);
    });

    it('lets the user keep the PC from switching to an account by itself', async () => {
      const t = nowS();
      const { token: me } = await pairAndSubscribe('/me');
      const accounts = [acct('hueyduck@example.com', 'huey', false, 3, 20, t + 3600, t + 86400)];
      await upload(accounts);
      assert.equal((await (await call('PUT', '/api/no-auto', { token: me, body: { account: 'huey', on: true } })).json()).no_auto, true);
      assert.equal((await (await upload(accounts)).json()).policies[0].no_auto, true);
      assert.equal((await (await call('GET', '/api/status', { token: me })).json()).accounts[0].caps.no_auto, true);
      await call('PUT', '/api/no-auto', { token: me, body: { account: 'huey', on: false } });
      assert.equal((await (await upload(accounts)).json()).policies[0].no_auto, false);
      assert.equal((await call('PUT', '/api/no-auto', { token: (await pairOwner('huey')).token, body: { account: 'huey', on: true } })).status, 403);
    });

    it('accepts push subscriptions from iPhone and Android browsers alike', () => {
      for (const ok of ['https://web.push.apple.com/QGv', 'https://fcm.googleapis.com/fcm/send/abc:def',
        'https://android.googleapis.com/gcm/send/abc', 'https://updates.push.services.mozilla.com/wpush/v2/x',
        'https://wns2-par02p.notify.windows.com/w/?token=x']) assert.equal(isAllowedEndpoint(ok), true, ok);
      for (const bad of ['http://fcm.googleapis.com/x', 'https://evil.example/fcm.googleapis.com', 'https://fcm.googleapis.com.evil.example/x'])
        assert.equal(isAllowedEndpoint(bad), false, bad);
    });

    it('records when a push reaches the phone and is shown (receipts from the service worker)', async () => {
      const t = nowS();
      await upload([acct('hueyduck@example.com', 'huey', false, 3, 20, t + 3600, t + 86400)]);
      const { sub } = await pairOwner('huey');
      const res = await ingest('/ingest/test-push', { for: 'huey' });
      assert.equal((await res.json()).delivered, 1);
      const note = decrypt(sub, to('/owner-huey').at(-1).body);
      assert.equal(note.title, 'Test from Nayef');
      const receipt = (body) => call('POST', '/api/receipt', { body });
      assert.equal((await receipt({ d: note.d, r: 'not-sent', stage: 'received' })).status, 404, 'only a push it was sent');
      assert.equal((await receipt({ d: note.d, r: note.r, stage: 'received' })).status, 204);
      assert.equal((await receipt({ d: note.d, r: note.r, stage: 'shown', info: 1 })).status, 204);
      const push = (await (await ingest('/ingest/health')).json()).phones.find((p) => p.for === 'huey').pushes.at(-1);
      assert.equal(push.title, 'Test from Nayef');
      assert.ok(push.received && push.shown);
      assert.equal(push.onScreen, 1);
    });

    it("keeps one phone per friend's account: pairing a new one ends the old", async () => {
      const t = nowS();
      await upload([acct('hueyduck@example.com', 'huey', false, 3, 20, t + 3600, t + 86400)]);
      const first = await pairOwner('huey');
      const second = await pairOwner('huey');
      assert.equal((await call('GET', '/api/status', { token: first.token })).status, 401);
      assert.equal((await call('GET', '/api/status', { token: second.token })).status, 200);
    });

    it('never warns an owner by itself, and lets the user tell them from the phone', async () => {
      const t = nowS();
      const list = (nayefPct) => [acct('nayeftest7@example.com', 'nayef', true, nayefPct, 20, t + 3600, t + 86400),
                                  acct('hueyduck@example.com', 'huey', false, 10, 5, t + 7200, t + 90000)];
      await upload(list(40));
      const { token: hueyToken, sub: hueySub } = await pairOwner('huey');
      const { token: meToken } = await pairAndSubscribe('/me');
      const huey = () => to('/owner-huey').map((s) => decrypt(hueySub, s.body));
      await upload(list(85));
      await upload(list(92));
      assert.equal(huey().filter((n) => /switch|about to use/.test(n.title)).length, 0, 'nothing automatic');

      const res = await call('POST', '/api/heads-up', { token: meToken, body: { account: 'huey' } });
      assert.equal(res.status, 200);
      assert.equal((await res.json()).delivered, 1);
      assert.deepEqual([huey().at(-1).title, huey().at(-1).body], ['Nayef is about to use your account', 'Tap for your usage.']);
      const wrap = await call('POST', '/api/wrap-up', { token: meToken, body: { account: 'huey' } });
      assert.equal((await wrap.json()).delivered, 1);
      assert.equal(huey().at(-1).title, 'Nayef is wrapping up a conversation');
      assert.equal((await call('POST', '/api/wrap-up', { token: meToken, body: { account: 'huey' } })).status, 429, 'double tap');
      // The user's phone sees whether huey can be notified.
      const phones = (await (await call('GET', '/api/status', { token: meToken })).json()).accounts.find((a) => a.name === 'huey').phones;
      assert.deepEqual(phones, { paired: 1, notifying: 1 });
      assert.equal((await call('POST', '/api/heads-up', { token: meToken, body: { account: 'huey' } })).status, 429, 'double tap');
      advance(11);
      assert.equal((await call('POST', '/api/heads-up', { token: meToken, body: { account: 'huey' } })).status, 200, 'again a moment later');
      // huey turns heads-ups off: none reach him.
      assert.equal((await call('PUT', '/api/prefs', { token: hueyToken, body: { heads_up: false } })).status, 200);
      advance(11);
      assert.equal((await (await call('POST', '/api/heads-up', { token: meToken, body: { account: 'huey' } })).json()).delivered, 0);
    });

    it("applies the lower of the owner's cap and the user's own", async () => {
      const t = nowS();
      const accounts = [acct('hueyduck@example.com', 'huey', true, 40, 10, t + 3600, t + 86400)];
      await upload(accounts);
      const { token: ownerToken, sub: ownerSub } = await pairOwner('huey');
      const { token: meToken } = await pairAndSubscribe('/me');
      const rule = async () => (await (await upload(accounts)).json()).policies[0];
      const ownerNotes = () => to('/owner-huey').map((x) => decrypt(ownerSub, x.body));

      assert.equal((await call('PUT', '/api/my-cap', { token: meToken, body: { account: 'huey', cap_five_hour: 50, cap_weekly: null } })).status, 200);
      assert.equal((await rule()).cap_five_hour, 50, 'mine when the owner set none');
      const ownerSees = await (await call('GET', '/api/status', { token: ownerToken })).json();
      assert.deepEqual(ownerSees.userCap, { five_hour: 50, weekly: null }, 'the owner sees it');
      assert.deepEqual([ownerNotes().at(-1).title, ownerNotes().at(-1).body], ['Nayef capped their use', 'On your account: 5h 50%.'], 'and is told');
      const told = ownerNotes().length;
      await call('PUT', '/api/my-cap', { token: meToken, body: { account: 'huey', cap_five_hour: 50, cap_weekly: null } });
      assert.equal(ownerNotes().length, told, 'nothing changed: no notification');
      await call('PUT', '/api/owner/policy', { token: ownerToken, body: { cap_five_hour: 30, cap_weekly: null, allow: true } });
      let r = await rule();
      assert.equal(r.cap_five_hour, 30, "the owner's when lower");
      assert.equal(r.owner_cap_five_hour, 30);
      await call('PUT', '/api/owner/policy', { token: ownerToken, body: { cap_five_hour: 80, cap_weekly: null, allow: true } });
      assert.equal((await rule()).cap_five_hour, 50, 'mine when lower');
      const status = await (await call('GET', '/api/status', { token: meToken })).json();
      assert.deepEqual(status.accounts[0].caps, { mine_five_hour: 50, mine_weekly: null, owner_five_hour: 80, owner_weekly: null, allow: true, reserve: null, auto_five_hour: false, no_auto: false });
      assert.equal((await call('PUT', '/api/my-cap', { token: ownerToken, body: { account: 'huey', cap_five_hour: 90 } })).status, 403);
      assert.equal((await call('PUT', '/api/my-cap', { token: meToken, body: { account: 'huey', cap_five_hour: null, cap_weekly: null } })).status, 200);
      assert.equal(ownerNotes().at(-1).title, 'Nayef removed their cap');
      assert.equal((await rule()).cap_five_hour, 80);
    });

    it('lets the user switch accounts from the phone; the PC picks it up once, while fresh', async () => {
      const t = nowS();
      await upload([acct('nayeftest7@example.com', 'nayef', true, 40, 20, t + 3600, t + 86400),
                    acct('hueyduck@example.com', 'huey', false, 10, 5, t + 7200, t + 90000)]);
      const { token: meToken } = await pairAndSubscribe('/me');
      const { token: hueyToken } = await pairOwner('huey');
      assert.equal((await call('POST', '/api/switch', { token: hueyToken, body: { account: 'huey' } })).status, 403);
      assert.equal((await call('POST', '/api/switch', { token: meToken, body: { account: 'spud' } })).status, 404);
      assert.equal((await call('POST', '/api/switch', { token: meToken, body: { account: 'huey' } })).status, 200);
      assert.equal((await (await call('GET', '/api/status', { token: meToken })).json()).pendingSwitch, 'huey');
      assert.deepEqual(await (await ingest('/ingest/commands', {})).json(), { switch_to: 'hueyduck@example.com' });
      assert.deepEqual(await (await ingest('/ingest/commands', {})).json(), {}, 'handed over once');
      await call('POST', '/api/switch', { token: meToken, body: { account: 'huey' } });
      advance(121);
      assert.deepEqual(await (await ingest('/ingest/commands', {})).json(), {}, 'a stale request is dropped');
      // The refresh button, from any phone (a friend's too): the PC is asked once.
      assert.equal((await call('POST', '/api/refresh', { token: hueyToken })).status, 200);
      await call('POST', '/api/refresh', { token: meToken });
      assert.deepEqual(await (await ingest('/ingest/commands', {})).json(), { refresh: true });
      assert.deepEqual(await (await ingest('/ingest/commands', {})).json(), {});
    });

    it("shows the PC's live token speed to the user, and to a friend only while their account is in use", async () => {
      const t = nowS();
      await ingest('/ingest/accounts', { user_name: 'Nayef', accounts: [acct('hueyduck@example.com', 'huey', true, 10, 5, t + 3600, t + 86400)],
        tokens: { rate: 207, history: [0, 12, 340, 207] } });
      const { token: meToken } = await pairAndSubscribe('/me');
      const { token: hueyToken } = await pairOwner('huey');
      const mine = await (await call('GET', '/api/status', { token: meToken })).json();
      assert.equal(mine.tokens.rate, 207);
      assert.deepEqual(mine.tokens.history, [0, 12, 340, 207]);
      // Huey's account is the one in use: his phone sees the speed (not the user's pile or history).
      const hueys = (await (await call('GET', '/api/status', { token: hueyToken })).json()).tokens;
      assert.equal(hueys.rate, 207);
      assert.deepEqual(hueys.history, []);

      // The 2-second live updates: newest wins, the user only, and nonsense is refused.
      const series = Array.from({ length: 300 }, (_, i) => i % 7);
      assert.equal((await ingest('/ingest/tokens', { rate: 151, count: 1640, working: true, pile: 3, series, history: [1, 2] })).status, 200);
      const live = await (await call('GET', '/api/tokens', { token: meToken })).json();
      assert.equal(live.tokens.rate, 151);
      assert.equal(live.tokens.count, 1640);
      assert.equal(live.tokens.working, true);
      assert.equal(live.tokens.pile, 3);
      assert.deepEqual(live.tokens.series, series);
      assert.equal((await (await call('GET', '/api/status', { token: meToken })).json()).tokens.rate, 151);
      // Huey's account is the one in use: his phone sees the speed (of his account only).
      const hueyLive = await (await call('GET', '/api/tokens', { token: hueyToken })).json();
      assert.equal(hueyLive.tokens.rate, 151);
      assert.equal(hueyLive.tokens.mode, 'speed');
      assert.equal((await ingest('/ingest/tokens', { rate: -1, history: [] })).status, 400);
      assert.equal((await ingest('/ingest/tokens', { rate: 1, count: -5, history: [] })).status, 400);
      await ingest('/ingest/accounts', { user_name: 'Nayef', accounts: [acct('hueyduck@example.com', 'huey', false, 10, 5, t + 3600, t + 86400)] });
      const off = (await (await call('GET', '/api/tokens', { token: hueyToken })).json()).tokens;
      assert.equal(off.rate, 0, 'not his account now: none of the tokens are his');
      assert.ok(off.series.some((v) => v > 0), 'his graph keeps the time his account was in use');
      assert.ok(off.series.slice(-1)[0] === 0 || off.series.length === 300);
      // ...but his own 5-hour usage over time is still there to graph.
      const usage = (await (await call('GET', '/api/status', { token: hueyToken })).json()).usageSeries;
      assert.ok(usage.length >= 1 && usage.at(-1)[1] === 10, 'his usage graph keeps going');
      assert.equal((await ingest('/ingest/tokens', { rate: 5, history: [], series: new Array(601).fill(1) })).status, 400);
    });

    it("keeps the user's own alerts away from owners", async () => {
      const t = nowS();
      await upload([acct('hueyduck@example.com', 'huey', true, 10, 5, t + 3600, t + 86400)]);
      await pairOwner('huey');
      const before = to('/owner-huey').length;
      const r = t - 5;
      await ingest('/ingest/event', { id: `five_hour_reset:${r}`, kind: 'five_hour_reset', reset_at: r });
      await ingest('/ingest/notice', { id: 'switch:x', title: 'Claude account switched', body: 'x' });
      await ingest('/ingest/test', {});
      assert.equal(to('/owner-huey').length, before);
    });
  });

  it('rejects malformed bodies cleanly', async () => {
    const res = await call('POST', '/ingest/usage', { token: env.INGEST_TOKEN, body: '{oops' });
    assert.equal(res.status, 400);
  });

  it('worker routes API calls to the hub and adds security headers', async () => {
    const workerEnv = {
      HUB: { idFromName: () => 'id', get: () => hub },
      ASSETS: { fetch: async () => new Response('<html>', { headers: { 'Content-Type': 'text/html' } }) },
    };
    const page = await worker.fetch(new Request('https://cloud.test/'), workerEnv);
    assert.match(page.headers.get('content-security-policy'), /default-src 'self'/);
    assert.equal(page.headers.get('x-frame-options'), 'DENY');
    const api = await worker.fetch(new Request('https://cloud.test/api/status'), workerEnv);
    assert.equal(api.status, 401);
    assert.equal(api.headers.get('x-content-type-options'), 'nosniff');
  });

  it('serves personal links that install an app which pairs itself', async () => {
    const assets = {
      '/': '<link rel="manifest" href="/manifest.webmanifest"><script src="/app.js"></script>',
      '/manifest.webmanifest': '{"id":"/","start_url":"/","name":"Claude Usage"}',
    };
    const workerEnv = {
      HUB: { idFromName: () => 'id', get: () => hub },
      ASSETS: { fetch: async (req) => new Response(assets[new URL(req.url).pathname] || 'nope') },
    };
    const page = await worker.fetch(new Request('https://cloud.test/join/abcd-efgh'), workerEnv);
    assert.equal(await page.text(), '<link rel="manifest" href="/join/ABCD-EFGH/manifest.webmanifest"><script src="/app.js"></script>');
    assert.match(page.headers.get('content-security-policy'), /default-src 'self'/);
    const manifest = await (await worker.fetch(new Request('https://cloud.test/join/ABCD-EFGH/manifest.webmanifest'), workerEnv)).json();
    assert.equal(manifest.start_url, '/join/ABCD-EFGH');
    assert.equal(manifest.name, 'Claude Usage');
    const other = await worker.fetch(new Request('https://cloud.test/join/<script>'), workerEnv);
    assert.equal(await other.text(), 'nope', 'anything else is a plain asset lookup');
  });
});
